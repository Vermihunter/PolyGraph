#!/usr/bin/env python3

import argparse
import hashlib
import json
import logging
import re
import sys
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Set, Tuple

import requests
from neo4j import GraphDatabase
from neo4j.exceptions import Neo4jError


logging.basicConfig(
    level=logging.INFO,
    format="%(levelname)s %(message)s"
)
logger = logging.getLogger("graph_loader")


# ----------------------------
# Constants / helpers
# ----------------------------

PROXY_NODE_TYPE = "Proxy"


def sanitize_name(name: str) -> str:
    """
    Convert a type/relationship name into a DB-safe identifier.
    Keeps letters, digits, underscore.
    Also guarantees the result is non-empty and does not start with a digit.
    """
    cleaned = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not cleaned:
        cleaned = "X"
    if cleaned[0].isdigit():
        cleaned = f"X_{cleaned}"
    return cleaned


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def build_proxy_node(local_source: str, remote_node: "NodeRecord") -> "NodeRecord":
    return NodeRecord(
        uuid=remote_node.uuid,
        type="Proxy",
        source=local_source,
        properties={
            "is_proxy": True,
            "remote-type": remote_node.type
        },
    )


@dataclass
class NodeRecord:
    uuid: str
    type: str
    source: str
    properties: Dict[str, Any]


@dataclass
class RelationshipRecord:
    uuid: str
    type: str
    source: str
    from_uuid: str
    to_uuid: str
    properties: Dict[str, Any]


def parse_nodes(raw_nodes: List[Dict[str, Any]]) -> List[NodeRecord]:
    nodes: List[NodeRecord] = []
    for item in raw_nodes:
        nodes.append(
            NodeRecord(
                uuid=item["uuid"],
                type=item["type"],
                source=item["source"],
                properties=item.get("properties", {}),
            )
        )
    return nodes


def parse_relationships(raw_rels: List[Dict[str, Any]]) -> List[RelationshipRecord]:
    rels: List[RelationshipRecord] = []
    for item in raw_rels:
        rels.append(
            RelationshipRecord(
                uuid=item["uuid"],
                type=item["type"],
                source=item["source"],
                from_uuid=item["from"],
                to_uuid=item["to"],
                properties=item.get("properties", {}),
            )
        )
    return rels


def relationship_touches_source(
        rel: RelationshipRecord,
        node_index: Dict[str, NodeRecord],
        source_name: str,
) -> bool:
    from_node = node_index.get(rel.from_uuid)
    to_node = node_index.get(rel.to_uuid)
    if from_node is None or to_node is None:
        return False
    return from_node.source == source_name or to_node.source == source_name


def is_cross_source_relationship(
        rel: RelationshipRecord,
        node_index: Dict[str, NodeRecord],
) -> bool:
    from_node = node_index.get(rel.from_uuid)
    to_node = node_index.get(rel.to_uuid)
    if from_node is None or to_node is None:
        return False
    return from_node.source != to_node.source


# ----------------------------
# Adapter base class
# ----------------------------

class DatabaseAdapter(ABC):
    def __init__(self, source_name: str):
        self.source_name = source_name

    @abstractmethod
    def initialize(self, nodes: List[NodeRecord], relationships: List[RelationshipRecord]) -> None:
        pass

    @abstractmethod
    def insert_node(self, node: NodeRecord) -> None:
        pass

    @abstractmethod
    def upsert_proxy_node(self, remote_node: NodeRecord) -> NodeRecord:
        pass

    @abstractmethod
    def insert_relationship(
            self,
            rel: RelationshipRecord,
            from_node: NodeRecord,
            to_node: NodeRecord,
    ) -> None:
        pass

    @abstractmethod
    def close(self) -> None:
        pass


# ----------------------------
# ArangoDB adapter
# ----------------------------

class ArangoDBAdapter(DatabaseAdapter):
    """
    Mapping:
    - each node type -> Arango document collection
    - each relationship type -> Arango edge collection
    - node document _key = uuid
    - edge document _key = uuid
    - edge _from/_to = <collection>/<uuid>

    Cross-database relationships:
    - remote nodes are represented as documents in the "Proxy" collection
    - the relationship is then inserted between the real local node and the proxy node
    """

    def __init__(
            self,
            base_url: str,
            username: str,
            password: str,
            database: str,
            source_name: str = "ArangoDB",
            timeout: int = 30,
    ):
        super().__init__(source_name)
        self.base_url = base_url.rstrip("/")
        self.username = username
        self.password = password
        self.database = database
        self.timeout = timeout
        self.session = requests.Session()
        self.session.auth = (self.username, self.password)
        self.session.headers.update({"Content-Type": "application/json"})
        self._known_document_collections: Set[str] = set()
        self._known_edge_collections: Set[str] = set()
        self._proxy_cache: Set[str] = set()

    def _db_api(self, path: str) -> str:
        return f"{self.base_url}/_db/{self.database}{path}"

    def _system_api(self, path: str) -> str:
        return f"{self.base_url}{path}"

    def _check_response(self, response: requests.Response, ok_codes: Tuple[int, ...], context: str) -> None:
        if response.status_code not in ok_codes:
            raise RuntimeError(
                f"{context} failed: HTTP {response.status_code} - {response.text}"
            )

    def ensure_database_exists(self) -> None:
        resp = self.session.get(self._system_api("/_api/database"), timeout=self.timeout)
        self._check_response(resp, (200,), "List ArangoDB databases")
        existing = resp.json().get("result", [])
        if self.database in existing:
            logger.info("ArangoDB database exists: %s", self.database)
            return

        payload = {"name": self.database}
        resp = self.session.post(
            self._system_api("/_api/database"),
            json=payload,
            timeout=self.timeout,
        )
        self._check_response(resp, (201,), f"Create ArangoDB database '{self.database}'")
        logger.info("Created ArangoDB database: %s", self.database)

    def ensure_collection(self, name: str, edge: bool = False) -> None:
        cache = self._known_edge_collections if edge else self._known_document_collections
        if name in cache:
            return

        payload = {"name": name}
        if edge:
            payload["type"] = 3

        resp = self.session.post(
            self._db_api("/_api/collection"),
            json=payload,
            timeout=self.timeout,
        )

        # 200/201 created, 409 already exists
        if resp.status_code in (200, 201):
            logger.info("Created ArangoDB collection: %s", name)
            cache.add(name)
            return
        if resp.status_code == 409:
            logger.info("ArangoDB collection exists: %s", name)
            cache.add(name)
            return

        raise RuntimeError(
            f"Create collection '{name}' failed: HTTP {resp.status_code} - {resp.text}"
        )

    def initialize(self, nodes: List[NodeRecord], relationships: List[RelationshipRecord]) -> None:
        self.ensure_database_exists()

        node_index = {n.uuid: n for n in nodes}

        node_types = sorted({sanitize_name(n.type) for n in nodes if n.source == self.source_name})
        rel_types = sorted({sanitize_name(r.type) for r in relationships if r.source == self.source_name})

        # A backend needs Proxy documents when it is the authoritative
        # relationship store for an edge whose endpoint node lives elsewhere.
        # Do NOT decide this from endpoint ownership alone; relationship.source
        # is the authoritative physical placement used by graph_db.
        needs_proxy_collection = any(
            r.source == self.source_name and (
                    node_index[r.from_uuid].source != self.source_name
                    or node_index[r.to_uuid].source != self.source_name
            )
            for r in relationships
            if r.from_uuid in node_index and r.to_uuid in node_index
        )
        if needs_proxy_collection:
            node_types.append(sanitize_name(PROXY_NODE_TYPE))

        for node_type in sorted(set(node_types)):
            self.ensure_collection(node_type, edge=False)

        for rel_type in rel_types:
            self.ensure_collection(rel_type, edge=True)

        graph_name = "UnifiedGraph"
        self.ensure_graph(graph_name, sorted(set(node_types)), rel_types)

    def ensure_graph(self, graph_name: str, vertex_collections: List[str], edge_collections: List[str]) -> None:
        edge_definitions = [
            {
                "collection": edge_col,
                "from": vertex_collections,
                "to": vertex_collections,
            }
            for edge_col in edge_collections
        ]

        payload = {
            "name": graph_name,
            "edgeDefinitions": edge_definitions,
            "orphanCollections": vertex_collections,
        }

        resp = self.session.post(
            self._db_api("/_api/gharial"),
            json=payload,
            timeout=self.timeout,
        )

        # 201 created, 409 already exists
        if resp.status_code in (200, 201, 202, 409):
            return

        raise RuntimeError(
            f"Create graph '{graph_name}' failed: HTTP {resp.status_code} - {resp.text}"
        )

    def insert_node(self, node: NodeRecord) -> None:
        collection = sanitize_name(node.type)
        self.ensure_collection(collection, edge=False)

        doc = {
            "_key": node.uuid,
            "uuid": node.uuid,
            "type": node.type,
            "source": node.source,
            **node.properties,
        }

        resp = self.session.post(
            self._db_api(f"/_api/document/{collection}"),
            json=doc,
            params={"overwriteMode": "replace"},
            timeout=self.timeout,
        )

        if resp.status_code not in (201, 202):
            raise RuntimeError(
                f"Insert Arango node {node.uuid} failed: HTTP {resp.status_code} - {resp.text}"
            )

    def upsert_proxy_node(self, remote_node: NodeRecord) -> NodeRecord:
        proxy_node = build_proxy_node(self.source_name, remote_node)
        if proxy_node.uuid not in self._proxy_cache:
            self.insert_node(proxy_node)
            self._proxy_cache.add(proxy_node.uuid)
            logger.info(
                "Created/updated ArangoDB proxy node %s for remote node %s (%s)",
                proxy_node.uuid,
                remote_node.uuid,
                remote_node.source,
            )
        return proxy_node

    def insert_relationship(
            self,
            rel: RelationshipRecord,
            from_node: NodeRecord,
            to_node: NodeRecord,
    ) -> None:
        edge_collection = sanitize_name(rel.type)
        from_collection = sanitize_name(from_node.type)
        to_collection = sanitize_name(to_node.type)

        self.ensure_collection(edge_collection, edge=True)
        self.ensure_collection(from_collection, edge=False)
        self.ensure_collection(to_collection, edge=False)

        doc = {
            "_key": rel.uuid,
            "_from": f"{from_collection}/{from_node.uuid}",
            "_to": f"{to_collection}/{to_node.uuid}",
            "uuid": rel.uuid,
            "type": rel.type,
            "source": rel.source,
            **rel.properties,
        }

        resp = self.session.post(
            self._db_api(f"/_api/document/{edge_collection}"),
            json=doc,
            params={"overwriteMode": "replace"},
            timeout=self.timeout,
        )

        if resp.status_code not in (201, 202):
            raise RuntimeError(
                f"Insert Arango relationship {rel.uuid} failed: HTTP {resp.status_code} - {resp.text}"
            )

    def close(self) -> None:
        self.session.close()


# ----------------------------
# Neo4j adapter
# ----------------------------

class Neo4jAdapter(DatabaseAdapter):
    """
    Mapping:
    - each node type -> Neo4j label
    - each relationship type -> Neo4j relationship type
    - MERGE on uuid

    Cross-database relationships:
    - remote nodes are represented as nodes with label :Proxy
    - the relationship is then inserted between the real local node and the proxy node
    """

    def __init__(
            self,
            uri: str,
            username: str,
            password: str,
            source_name: str = "Neo4J",
    ):
        super().__init__(source_name)
        self.driver = GraphDatabase.driver(uri, auth=(username, password))
        self._known_constraints: Set[str] = set()
        self._proxy_cache: Set[str] = set()

    def ensure_constraint(self, label: str) -> None:
        if label in self._known_constraints:
            return

        constraint_name = sanitize_name(f"{label.lower()}_uuid_unique")
        cypher = (
            f"CREATE CONSTRAINT {constraint_name} IF NOT EXISTS "
            f"FOR (n:{label}) REQUIRE n.uuid IS UNIQUE"
        )

        with self.driver.session() as session:
            try:
                session.run(cypher)
                self._known_constraints.add(label)
                logger.info("Neo4j constraint ready for label: %s", label)
            except Neo4jError as e:
                raise RuntimeError(f"Neo4j constraint creation failed for {label}: {e}") from e

    def initialize(self, nodes: List[NodeRecord], relationships: List[RelationshipRecord]) -> None:
        node_index = {n.uuid: n for n in nodes}

        node_types = {sanitize_name(n.type) for n in nodes if n.source == self.source_name}

        # A backend needs Proxy nodes when it is the authoritative
        # relationship store for an edge whose endpoint node lives elsewhere.
        # relationship.source must match the C++ Placement backend.
        needs_proxy_label = any(
            r.source == self.source_name and (
                    node_index[r.from_uuid].source != self.source_name
                    or node_index[r.to_uuid].source != self.source_name
            )
            for r in relationships
            if r.from_uuid in node_index and r.to_uuid in node_index
        )
        if needs_proxy_label:
            node_types.add(sanitize_name(PROXY_NODE_TYPE))

        for node_type in sorted(node_types):
            self.ensure_constraint(node_type)

    def insert_node(self, node: NodeRecord) -> None:
        label = sanitize_name(node.type)
        self.ensure_constraint(label)

        props = {
            "uuid": node.uuid,
            "type": node.type,
            "source": node.source,
            **node.properties,
        }

        cypher = f"""
        MERGE (n:{label} {{uuid: $uuid}})
        SET n += $props
        """

        with self.driver.session() as session:
            session.run(cypher, uuid=node.uuid, props=props)

    def upsert_proxy_node(self, remote_node: NodeRecord) -> NodeRecord:
        proxy_node = build_proxy_node(self.source_name, remote_node)
        if proxy_node.uuid not in self._proxy_cache:
            self.insert_node(proxy_node)
            self._proxy_cache.add(proxy_node.uuid)
            logger.info(
                "Created/updated Neo4j proxy node %s for remote node %s (%s)",
                proxy_node.uuid,
                remote_node.uuid,
                remote_node.source,
            )
        return proxy_node

    def insert_relationship(
            self,
            rel: RelationshipRecord,
            from_node: NodeRecord,
            to_node: NodeRecord,
    ) -> None:
        from_label = sanitize_name(from_node.type)
        to_label = sanitize_name(to_node.type)
        rel_type = sanitize_name(rel.type)

        self.ensure_constraint(from_label)
        self.ensure_constraint(to_label)

        props = {
            "uuid": rel.uuid,
            "type": rel.type,
            "source": rel.source,
            **rel.properties,
        }

        cypher = f"""
        MATCH (a:{from_label} {{uuid: $from_uuid}})
        MATCH (b:{to_label} {{uuid: $to_uuid}})
        MERGE (a)-[r:{rel_type} {{uuid: $rel_uuid}}]->(b)
        SET r += $props
        """

        with self.driver.session() as session:
            session.run(
                cypher,
                from_uuid=from_node.uuid,
                to_uuid=to_node.uuid,
                rel_uuid=rel.uuid,
                props=props,
            )

    def close(self) -> None:
        self.driver.close()


# ----------------------------
# Registry / dispatcher
# ----------------------------

class AdapterRegistry:
    def __init__(self):
        self._adapters: Dict[str, DatabaseAdapter] = {}

    def register(self, source_name: str, adapter: DatabaseAdapter) -> None:
        self._adapters[source_name] = adapter

    def get(self, source_name: str) -> Optional[DatabaseAdapter]:
        return self._adapters.get(source_name)

    def items(self):
        return self._adapters.items()

    def close_all(self) -> None:
        for adapter in self._adapters.values():
            adapter.close()


# ----------------------------
# Main loading flow
# ----------------------------

def initialize_databases(
        adapters: AdapterRegistry,
        nodes: List[NodeRecord],
        relationships: List[RelationshipRecord]
) -> None:
    for source_name, adapter in adapters.items():
        logger.info("Initializing database for source: %s", source_name)
        adapter.initialize(nodes, relationships)


def insert_nodes(
        adapters: AdapterRegistry,
        nodes: List[NodeRecord],
) -> None:
    for node in nodes:
        adapter = adapters.get(node.source)
        if adapter is None:
            logger.warning("No adapter registered for node source %s, skipping node %s", node.source, node.uuid)
            continue
        adapter.insert_node(node)


def insert_authoritative_relationship(
        adapters: AdapterRegistry,
        rel: RelationshipRecord,
        from_node: NodeRecord,
        to_node: NodeRecord,
) -> None:
    """
    Insert the relationship into rel.source, because rel.source is the
    authoritative physical placement used by the C++ PlacementCatalog.

    Endpoint source tells us where the real node lives. If an endpoint lives
    outside the authoritative relationship backend, represent it there as a
    Proxy node with the same uuid and remote-type metadata.

    GPU is not loaded through Neo4j/ArangoDB; GPU relationships are consumed
    directly from the JSON file by GpuProjectionBuilder.
    """
    if rel.source == "GPU":
        return

    adapter = adapters.get(rel.source)
    if adapter is None:
        logger.warning(
            "No adapter registered for authoritative relationship source %s, skipping relationship %s",
            rel.source,
            rel.uuid,
        )
        return

    local_from = (
        from_node
        if from_node.source == rel.source
        else adapter.upsert_proxy_node(from_node)
    )
    local_to = (
        to_node
        if to_node.source == rel.source
        else adapter.upsert_proxy_node(to_node)
    )

    adapter.insert_relationship(rel, local_from, local_to)

def insert_relationships(
        adapters: AdapterRegistry,
        relationships: List[RelationshipRecord],
        node_index: Dict[str, NodeRecord],
) -> None:
    for rel in relationships:
        from_node = node_index.get(rel.from_uuid)
        to_node = node_index.get(rel.to_uuid)

        if from_node is None or to_node is None:
            logger.warning(
                "Skipping relationship %s because endpoint node is missing in input",
                rel.uuid,
            )
            continue

        insert_authoritative_relationship(adapters, rel, from_node, to_node)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Load generated graph JSON into Neo4j / ArangoDB based on source field, including cross-database proxy edges."
    )
    parser.add_argument(
        "input_json",
        help="Path to generated data JSON (small/medium/large)"
    )

    # ArangoDB config
    parser.add_argument("--arango-url", default="http://localhost:8529")
    parser.add_argument("--arango-user", default="root")
    parser.add_argument("--arango-password", default="rootpassword")
    parser.add_argument("--arango-db", default="university")

    # Neo4j config
    parser.add_argument("--neo4j-uri", default="bolt://localhost:7687")
    parser.add_argument("--neo4j-user", default="neo4j")
    parser.add_argument("--neo4j-password", default="password")

    args = parser.parse_args()

    adapters: Optional[AdapterRegistry] = None

    try:
        data = load_json(args.input_json)
        nodes = parse_nodes(data.get("nodes", []))
        relationships = parse_relationships(data.get("relationships", []))
        node_index = {n.uuid: n for n in nodes}

        adapters = AdapterRegistry()

        # Register adapters here.
        # Easy to extend later: add another adapter + register it.
        adapters.register(
            "ArangoDB",
            ArangoDBAdapter(
                base_url=args.arango_url,
                username=args.arango_user,
                password=args.arango_password,
                database=args.arango_db,
                source_name="ArangoDB",
            )
        )
        adapters.register(
            "Neo4J",
            Neo4jAdapter(
                uri=args.neo4j_uri,
                username=args.neo4j_user,
                password=args.neo4j_password,
                source_name="Neo4J",
            )
        )

        initialize_databases(adapters, nodes, relationships)

        logger.info("Inserting %d real nodes", len(nodes))
        insert_nodes(adapters, nodes)

        logger.info("Inserting %d relationships", len(relationships))
        insert_relationships(adapters, relationships, node_index)

        logger.info("Done")
        return 0

    except Exception as e:
        logger.exception("Load failed: %s", e)
        return 1

    finally:
        if adapters is not None:
            adapters.close_all()


if __name__ == "__main__":
    sys.exit(main())