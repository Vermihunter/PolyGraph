#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import logging
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from itertools import islice
from pathlib import Path
from typing import Iterable, Iterator, TypeVar

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

import requests
from neo4j import GraphDatabase

from tools.graphio import BackendLayout, NodeRecord, RelationshipRecord, open_graph_reader

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
LOGGER = logging.getLogger("graph_loader")
PROXY_NODE_TYPE = "Proxy"
T = TypeVar("T")


@dataclass(frozen=True)
class EndpointInfo:
    type: str
    source: str


def sanitize_name(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not cleaned:
        cleaned = "X"
    if cleaned[0].isdigit():
        cleaned = f"X_{cleaned}"
    return cleaned


def batched(values: Iterable[T], size: int) -> Iterator[list[T]]:
    if size <= 0:
        raise ValueError("batch size must be positive")
    iterator = iter(values)
    while batch := list(islice(iterator, size)):
        yield batch


def build_proxy_node(local_source: str, uuid: str, remote: EndpointInfo) -> NodeRecord:
    return NodeRecord(
        uuid=uuid,
        type=PROXY_NODE_TYPE,
        source=local_source,
        properties={"is_proxy": True, "remote-type": remote.type, "remote-source": remote.source},
    )


class Neo4jAdapter:
    def __init__(self, uri: str, username: str, password: str, *, batch_size: int):
        self.source_name = "Neo4J"
        self.driver = GraphDatabase.driver(uri, auth=(username, password))
        self.batch_size = batch_size

    def close(self) -> None:
        self.driver.close()

    def reset(self) -> None:
        with self.driver.session() as session:
            session.run("MATCH (n) DETACH DELETE n").consume()

    def prepare(self, node_types: Iterable[str], _relationship_types: Iterable[str]) -> None:
        with self.driver.session() as session:
            for type_name in sorted(set(node_types)):
                label = sanitize_name(type_name)
                constraint = sanitize_name(f"{label.lower()}_uuid_unique")
                session.run(
                    f"CREATE CONSTRAINT {constraint} IF NOT EXISTS FOR (n:{label}) REQUIRE n.uuid IS UNIQUE"
                ).consume()

    def insert_nodes(self, nodes: list[NodeRecord]) -> None:
        by_type: dict[str, list[dict]] = defaultdict(list)
        for node in nodes:
            by_type[node.type].append({
                "uuid": node.uuid,
                "props": {"uuid": node.uuid, "type": node.type, "source": node.source, **node.properties},
            })
        with self.driver.session() as session:
            for type_name, rows in sorted(by_type.items()):
                label = sanitize_name(type_name)
                query = f"UNWIND $rows AS row MERGE (n:{label} {{uuid: row.uuid}}) SET n += row.props"
                session.run(query, rows=rows).consume()

    def insert_relationships(
        self,
        relationships: list[RelationshipRecord],
        local_endpoint_types: dict[str, str],
    ) -> None:
        grouped: dict[tuple[str, str, str], list[dict]] = defaultdict(list)
        for relationship in relationships:
            grouped[(
                relationship.type,
                local_endpoint_types[relationship.from_uuid],
                local_endpoint_types[relationship.to_uuid],
            )].append({
                "uuid": relationship.uuid,
                "from_uuid": relationship.from_uuid,
                "to_uuid": relationship.to_uuid,
                "props": {
                    "uuid": relationship.uuid,
                    "type": relationship.type,
                    "source": relationship.source,
                    **relationship.properties,
                },
            })
        with self.driver.session() as session:
            for (rel_type, from_type, to_type), rows in sorted(grouped.items()):
                query = f"""
                UNWIND $rows AS row
                MATCH (a:{sanitize_name(from_type)} {{uuid: row.from_uuid}})
                MATCH (b:{sanitize_name(to_type)} {{uuid: row.to_uuid}})
                MERGE (a)-[r:{sanitize_name(rel_type)} {{uuid: row.uuid}}]->(b)
                SET r += row.props
                """
                session.run(query, rows=rows).consume()


class ArangoDBAdapter:
    def __init__(self, base_url: str, username: str, password: str, database: str, *, batch_size: int):
        self.source_name = "ArangoDB"
        self.base_url = base_url.rstrip("/")
        self.database = database
        self.batch_size = batch_size
        self.session = requests.Session()
        self.session.auth = (username, password)
        self.session.headers.update({"Content-Type": "application/json"})

    def close(self) -> None:
        self.session.close()

    def system_api(self, path: str) -> str:
        return f"{self.base_url}{path}"

    def db_api(self, path: str) -> str:
        return f"{self.base_url}/_db/{self.database}{path}"

    @staticmethod
    def check(response: requests.Response, ok: tuple[int, ...], context: str) -> dict:
        if response.status_code not in ok:
            raise RuntimeError(f"{context}: HTTP {response.status_code}: {response.text}")
        return response.json() if response.text else {}

    def ensure_database(self) -> None:
        response = self.session.get(self.system_api("/_api/database"), timeout=60)
        payload = self.check(response, (200,), "List Arango databases")
        if self.database not in payload.get("result", []):
            response = self.session.post(self.system_api("/_api/database"), json={"name": self.database}, timeout=60)
            self.check(response, (201,), "Create Arango database")

    def reset(self) -> None:
        response = self.session.delete(self.system_api(f"/_api/database/{self.database}"), timeout=300)
        if response.status_code not in (200, 202, 404):
            raise RuntimeError(f"Delete Arango database: HTTP {response.status_code}: {response.text}")
        response = self.session.post(self.system_api("/_api/database"), json={"name": self.database}, timeout=300)
        self.check(response, (201,), "Create Arango database")

    def ensure_collection(self, name: str, *, edge: bool) -> None:
        response = self.session.post(
            self.db_api("/_api/collection"),
            json={"name": sanitize_name(name), "type": 3 if edge else 2},
            timeout=120,
        )
        if response.status_code not in (200, 201, 409):
            raise RuntimeError(f"Create Arango collection {name}: HTTP {response.status_code}: {response.text}")

    def prepare(self, node_types: Iterable[str], relationship_types: Iterable[str]) -> None:
        self.ensure_database()
        vertices = sorted({sanitize_name(name) for name in node_types})
        edges = sorted({sanitize_name(name) for name in relationship_types})
        for name in vertices:
            self.ensure_collection(name, edge=False)
        for name in edges:
            self.ensure_collection(name, edge=True)
        edge_definitions = [{"collection": edge, "from": vertices, "to": vertices} for edge in edges]
        response = self.session.post(
            self.db_api("/_api/gharial"),
            json={"name": "UnifiedGraph", "edgeDefinitions": edge_definitions, "orphanCollections": []},
            timeout=300,
        )
        if response.status_code not in (200, 201, 202, 409):
            raise RuntimeError(f"Create Arango graph: HTTP {response.status_code}: {response.text}")

    def import_batch(self, collection: str, documents: list[dict]) -> None:
        response = self.session.post(
            self.db_api("/_api/import"),
            params={
                "collection": sanitize_name(collection),
                "type": "array",
                "onDuplicate": "replace",
                "complete": "true",
            },
            data=json.dumps(documents, ensure_ascii=False),
            timeout=900,
        )
        payload = self.check(response, (201,), f"Import into Arango collection {collection}")
        if payload.get("errors", 0):
            raise RuntimeError(f"Arango import into {collection} reported errors: {payload}")

    def insert_nodes(self, nodes: list[NodeRecord]) -> None:
        by_type: dict[str, list[dict]] = defaultdict(list)
        for node in nodes:
            by_type[node.type].append({
                "_key": node.uuid,
                "uuid": node.uuid,
                "type": node.type,
                "source": node.source,
                **node.properties,
            })
        for type_name, documents in sorted(by_type.items()):
            self.import_batch(type_name, documents)

    def insert_relationships(
        self,
        relationships: list[RelationshipRecord],
        local_endpoint_types: dict[str, str],
    ) -> None:
        by_type: dict[str, list[dict]] = defaultdict(list)
        for relationship in relationships:
            by_type[relationship.type].append({
                "_key": relationship.uuid,
                "_from": f"{sanitize_name(local_endpoint_types[relationship.from_uuid])}/{relationship.from_uuid}",
                "_to": f"{sanitize_name(local_endpoint_types[relationship.to_uuid])}/{relationship.to_uuid}",
                "uuid": relationship.uuid,
                "type": relationship.type,
                "source": relationship.source,
                **relationship.properties,
            })
        for type_name, documents in sorted(by_type.items()):
            self.import_batch(type_name, documents)


def graph_routing_layout(input_graph: str) -> BackendLayout:
    metadata = open_graph_reader(input_graph).metadata()
    configured = metadata.get("uuidRouting")
    if not configured:
        raise ValueError(
            "Graph has no uuidRouting metadata; regenerate or migrate UUIDs before loading"
        )
    return BackendLayout(tuple(configured))


def build_endpoint_directory(input_graph: str, routing_layout: BackendLayout) -> dict[str, EndpointInfo]:
    directory: dict[str, EndpointInfo] = {}
    for node in open_graph_reader(input_graph).iter_nodes():
        if node.uuid in directory:
            raise ValueError(f"Duplicate node UUID: {node.uuid}")
        directory[node.uuid] = EndpointInfo(type=node.type, source=routing_layout.decode_source(node.uuid))
    return directory


def relationship_stored_on(
    relationship: RelationshipRecord,
    source: str,
    endpoint_directory: dict[str, EndpointInfo],
    routing_layout: BackendLayout,
) -> bool:
    try:
        homes = {
            routing_layout.decode_source(relationship.uuid),
            endpoint_directory[relationship.from_uuid].source,
            endpoint_directory[relationship.to_uuid].source,
        }
    except KeyError as exc:
        raise ValueError(
            f"Relationship {relationship.uuid} references missing endpoint {exc.args[0]}"
        ) from exc
    return source in homes


def inspect_source(
    input_graph: str,
    source: str,
    endpoint_directory: dict[str, EndpointInfo],
    routing_layout: BackendLayout,
) -> tuple[set[str], set[str], set[str], dict[str, str]]:
    node_types = {endpoint.type for endpoint in endpoint_directory.values() if endpoint.source == source}
    relationship_types: set[str] = set()
    proxy_ids: set[str] = set()
    local_endpoint_types: dict[str, str] = {
        uuid: endpoint.type for uuid, endpoint in endpoint_directory.items() if endpoint.source == source
    }

    for relationship in open_graph_reader(input_graph).iter_relationships():
        if not relationship_stored_on(relationship, source, endpoint_directory, routing_layout):
            continue
        relationship_types.add(relationship.type)
        for endpoint_uuid in (relationship.from_uuid, relationship.to_uuid):
            endpoint = endpoint_directory.get(endpoint_uuid)
            if endpoint is None:
                raise ValueError(
                    f"Relationship {relationship.uuid} references missing endpoint {endpoint_uuid}"
                )
            if endpoint.source != source:
                proxy_ids.add(endpoint_uuid)
                local_endpoint_types[endpoint_uuid] = PROXY_NODE_TYPE

    if proxy_ids:
        node_types.add(PROXY_NODE_TYPE)
    return node_types, relationship_types, proxy_ids, local_endpoint_types


def load_source(
    *,
    input_graph: str,
    source: str,
    adapter: Neo4jAdapter | ArangoDBAdapter,
    endpoint_directory: dict[str, EndpointInfo],
    routing_layout: BackendLayout,
    reset: bool,
    batch_size: int,
) -> None:
    node_types, relationship_types, proxy_ids, local_endpoint_types = inspect_source(
        input_graph, source, endpoint_directory, routing_layout
    )
    if not node_types and not relationship_types:
        LOGGER.info("Skipping %s: no records assigned to this backend", source)
        return

    LOGGER.info(
        "Preparing %s: %d node types, %d relationship types, %d proxy nodes",
        source,
        len(node_types),
        len(relationship_types),
        len(proxy_ids),
    )
    if reset:
        adapter.reset()
    adapter.prepare(node_types, relationship_types)

    local_nodes = (
        node
        for node in open_graph_reader(input_graph).iter_nodes()
        if routing_layout.decode_source(node.uuid) == source
    )
    node_count = 0
    for batch in batched(local_nodes, batch_size):
        adapter.insert_nodes(batch)
        node_count += len(batch)

    proxy_nodes = (
        build_proxy_node(source, uuid, endpoint_directory[uuid]) for uuid in sorted(proxy_ids)
    )
    for batch in batched(proxy_nodes, batch_size):
        adapter.insert_nodes(batch)
        node_count += len(batch)

    local_relationships = (
        relationship
        for relationship in open_graph_reader(input_graph).iter_relationships()
        if relationship_stored_on(relationship, source, endpoint_directory, routing_layout)
    )
    relationship_count = 0
    for batch in batched(local_relationships, batch_size):
        adapter.insert_relationships(batch, local_endpoint_types)
        relationship_count += len(batch)

    LOGGER.info("Loaded %s: %d nodes including proxies, %d relationships", source, node_count, relationship_count)


def inspect_gpu_projection(
    input_graph: str,
    endpoint_directory: dict[str, EndpointInfo],
    routing_layout: BackendLayout,
) -> tuple[int, int, int]:
    gpu_nodes = sum(1 for endpoint in endpoint_directory.values() if endpoint.source == "GPU")
    projection_vertices = {
        uuid for uuid, endpoint in endpoint_directory.items() if endpoint.source == "GPU"
    }
    gpu_edges = 0
    for relationship in open_graph_reader(input_graph).iter_relationships():
        from_endpoint = endpoint_directory.get(relationship.from_uuid)
        to_endpoint = endpoint_directory.get(relationship.to_uuid)
        if from_endpoint is None or to_endpoint is None:
            missing = relationship.from_uuid if from_endpoint is None else relationship.to_uuid
            raise ValueError(f"Relationship {relationship.uuid} references missing endpoint {missing}")
        if (
            routing_layout.decode_source(relationship.uuid) == "GPU"
            or from_endpoint.source == "GPU"
            or to_endpoint.source == "GPU"
        ):
            gpu_edges += 1
            projection_vertices.add(relationship.from_uuid)
            projection_vertices.add(relationship.to_uuid)
    return gpu_nodes, len(projection_vertices), gpu_edges


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Stream and batch-load CPGDB JSON/JSONL graph data into Neo4j and ArangoDB "
            "according to the backend code encoded in each UUID."
        )
    )
    parser.add_argument("input_graph", help="Path to graph .json, .jsonl, or .ndjson")
    parser.add_argument("--batch-size", type=int, default=5000)
    parser.add_argument("--reset", action="store_true", help="Clear each used database before loading")
    parser.add_argument(
        "--only-source", choices=["Neo4J", "ArangoDB", "GPU"], action="append", default=[]
    )
    parser.add_argument("--arango-url", default="http://localhost:8529")
    parser.add_argument("--arango-user", default="root")
    parser.add_argument("--arango-password", default="rootpassword")
    parser.add_argument("--arango-db", default="university")
    parser.add_argument("--neo4j-uri", default="bolt://localhost:7687")
    parser.add_argument("--neo4j-user", default="neo4j")
    parser.add_argument("--neo4j-password", default="password")
    args = parser.parse_args()

    if args.batch_size <= 0:
        parser.error("--batch-size must be positive")

    routing_layout = graph_routing_layout(args.input_graph)
    endpoint_directory = build_endpoint_directory(args.input_graph, routing_layout)
    selected = set(args.only_source or routing_layout.backends)
    adapters: dict[str, Neo4jAdapter | ArangoDBAdapter] = {}
    if "Neo4J" in selected:
        adapters["Neo4J"] = Neo4jAdapter(
            args.neo4j_uri, args.neo4j_user, args.neo4j_password, batch_size=args.batch_size
        )
    if "ArangoDB" in selected:
        adapters["ArangoDB"] = ArangoDBAdapter(
            args.arango_url,
            args.arango_user,
            args.arango_password,
            args.arango_db,
            batch_size=args.batch_size,
        )

    if "GPU" in selected:
        gpu_nodes, projection_vertices, gpu_edges = inspect_gpu_projection(
            args.input_graph, endpoint_directory, routing_layout
        )
        LOGGER.info(
            "GPU runtime projection: %d owned nodes, %d total projection vertices, %d edges",
            gpu_nodes,
            projection_vertices,
            gpu_edges,
        )

    try:
        for source, adapter in adapters.items():
            load_source(
                input_graph=args.input_graph,
                source=source,
                adapter=adapter,
                endpoint_directory=endpoint_directory,
                routing_layout=routing_layout,
                reset=args.reset,
                batch_size=args.batch_size,
            )
    finally:
        for adapter in adapters.values():
            adapter.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
