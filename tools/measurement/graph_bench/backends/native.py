from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path
from typing import Any, DefaultDict, Iterable, Optional, Sequence

import requests

from ..common import sanitize_db_identifier
from ..graph_data import GraphData, NodeRecord, RelationshipRecord

GRAPH_NODE_LABEL = "__GraphNode"
GRAPH_NAME = "UnifiedGraph"


def _cypher_ident(name: str) -> str:
    return f"`{sanitize_db_identifier(name)}`"


def _entity_label() -> str:
    return _cypher_ident(GRAPH_NODE_LABEL)


def _chunks(items: Sequence[dict[str, Any]], size: int) -> Iterable[Sequence[dict[str, Any]]]:
    for i in range(0, len(items), size):
        yield items[i:i + size]


def relationship_filter_from_schema(schema_path: str | Path) -> str:
    with open(schema_path, "r", encoding="utf-8") as f:
        schema = json.load(f)
    return "|".join(sanitize_db_identifier(r["name"]) for r in schema.get("relations", []))


def write_bfs_csv(path: str | Path, rows: Iterable[tuple[str, int]]) -> None:
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    ordered = sorted(rows, key=lambda x: (x[1], x[0]))
    with open(out, "w", encoding="utf-8") as f:
        f.write("vertex_id;distance\n")
        for vertex_id, distance in ordered:
            f.write(f"{vertex_id};{distance}\n")


class Neo4jNativeClient:
    """Native full-replica Neo4j loader + APOC BFS query client."""

    def __init__(self, endpoint: str, user: str, password: str, *, batch_size: int = 10000):
        self.endpoint = endpoint
        self.batch_size = batch_size
        self.session = requests.Session()
        if user:
            self.session.auth = (user, password)
        self.session.headers.update({"Content-Type": "application/json"})

    def close(self) -> None:
        self.session.close()

    def tx(self, statements: list[dict[str, Any]], *, timeout: int = 900) -> dict[str, Any]:
        response = self.session.post(self.endpoint, json={"statements": statements}, timeout=timeout)
        if response.status_code != 200:
            raise RuntimeError(f"Neo4j HTTP {response.status_code}: {response.text}")
        payload = response.json()
        if payload.get("errors"):
            raise RuntimeError(f"Neo4j returned errors: {json.dumps(payload['errors'], indent=2)}")
        return payload

    def reset(self) -> None:
        self.tx([{"statement": "MATCH (n) DETACH DELETE n"}], timeout=900)

    def create_constraints(self) -> None:
        self.tx([{"statement": f"""
            CREATE CONSTRAINT graphnode_uuid_unique IF NOT EXISTS
            FOR (n:{_entity_label()}) REQUIRE n.uuid IS UNIQUE
        """}], timeout=300)

    def load_full_graph(self, graph: GraphData) -> None:
        self.reset()

        nodes_by_type: DefaultDict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in graph.nodes:
            props = {**node.properties, "uuid": node.uuid, "type": node.type, "source": node.source}
            nodes_by_type[node.type].append({"props": props})

        for node_type, rows in sorted(nodes_by_type.items()):
            label = _cypher_ident(node_type)
            statement = f"""
                UNWIND $rows AS row
                CREATE (n:{_entity_label()}:{label})
                SET n = row.props
            """
            for batch in _chunks(rows, self.batch_size):
                self.tx([{"statement": statement, "parameters": {"rows": list(batch)}}], timeout=900)

        self.create_constraints()

        rels_by_type: DefaultDict[str, list[dict[str, Any]]] = defaultdict(list)
        for rel in graph.relationships:
            props = {**rel.properties, "uuid": rel.uuid, "type": rel.type, "source": rel.source}
            rels_by_type[rel.type].append({"from_uuid": rel.from_uuid, "to_uuid": rel.to_uuid, "props": props})

        for rel_type, rows in sorted(rels_by_type.items()):
            rtype = _cypher_ident(rel_type)
            statement = f"""
                UNWIND $rows AS row
                MATCH (a:{_entity_label()} {{uuid: row.from_uuid}})
                MATCH (b:{_entity_label()} {{uuid: row.to_uuid}})
                CREATE (a)-[r:{rtype}]->(b)
                SET r = row.props
            """
            for batch in _chunks(rows, self.batch_size):
                self.tx([{"statement": statement, "parameters": {"rows": list(batch)}}], timeout=900)

    def bfs(self, start_uuid: str, relationship_filter: str, max_depth: int, output_csv: str | Path) -> int:
        statement = f"""
            MATCH (start:{_entity_label()} {{uuid: $startUuid}})
            CALL apoc.path.expandConfig(start, {{
              minLevel: 0,
              maxLevel: $maxDepth,
              relationshipFilter: $relationshipFilter,
              bfs: true,
              uniqueness: 'NODE_GLOBAL'
            }}) YIELD path
            WITH last(nodes(path)) AS node, length(path) AS distance
            RETURN node.uuid AS vertex_id, distance AS distance
        """
        payload = self.tx([{
            "statement": statement,
            "parameters": {
                "startUuid": start_uuid,
                "maxDepth": max_depth,
                "relationshipFilter": relationship_filter,
            },
        }], timeout=900)
        rows = [(row["row"][0], int(row["row"][1])) for row in payload["results"][0]["data"]]
        write_bfs_csv(output_csv, rows)
        return len(rows)


class ArangoNativeClient:
    """Native full-replica ArangoDB loader + named-graph BFS traversal client."""

    def __init__(self, base_url: str, user: str, password: str, database: str, *, batch_size: int = 10000):
        self.base_url = base_url.rstrip("/")
        self.database = database
        self.batch_size = batch_size
        self.session = requests.Session()
        self.session.auth = (user, password)
        self.session.headers.update({"Content-Type": "application/json"})

    def close(self) -> None:
        self.session.close()

    def system_api(self, path: str) -> str:
        return f"{self.base_url}{path}"

    def db_api(self, path: str) -> str:
        return f"{self.base_url}/_db/{self.database}{path}"

    def check(self, response: requests.Response, ok: tuple[int, ...], context: str) -> dict[str, Any]:
        if response.status_code not in ok:
            raise RuntimeError(f"{context} failed: HTTP {response.status_code}: {response.text}")
        return response.json() if response.text else {}

    def reset_database(self) -> None:
        response = self.session.delete(self.system_api(f"/_api/database/{self.database}"), timeout=300)
        if response.status_code not in (200, 202, 404):
            raise RuntimeError(f"Delete Arango database failed: HTTP {response.status_code}: {response.text}")
        response = self.session.post(self.system_api("/_api/database"), json={"name": self.database}, timeout=300)
        self.check(response, (201,), "Create Arango database")

    def ensure_collection(self, name: str, *, edge: bool = False) -> None:
        payload: dict[str, Any] = {"name": sanitize_db_identifier(name)}
        if edge:
            payload["type"] = 3
        response = self.session.post(self.db_api("/_api/collection"), json=payload, timeout=300)
        if response.status_code not in (200, 201, 409):
            raise RuntimeError(f"Create Arango collection {name} failed: HTTP {response.status_code}: {response.text}")

    def load_full_graph(self, graph: GraphData) -> None:
        self.reset_database()
        vertex_collections = sorted({sanitize_db_identifier(n.type) for n in graph.nodes})
        edge_collections = sorted({sanitize_db_identifier(r.type) for r in graph.relationships})
        for collection in vertex_collections:
            self.ensure_collection(collection, edge=False)
        for collection in edge_collections:
            self.ensure_collection(collection, edge=True)

        nodes_by_type: DefaultDict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in graph.nodes:
            doc = {**node.properties, "_key": node.uuid, "uuid": node.uuid, "type": node.type, "source": node.source}
            nodes_by_type[node.type].append(doc)

        for node_type, docs in sorted(nodes_by_type.items()):
            collection = sanitize_db_identifier(node_type)
            for batch in _chunks(docs, self.batch_size):
                response = self.session.post(
                    self.db_api(f"/_api/document/{collection}"),
                    params={"overwriteMode": "replace"},
                    json=list(batch),
                    timeout=900,
                )
                self.check(response, (201, 202), f"Insert Arango nodes into {collection}")

        rels_by_type: DefaultDict[str, list[dict[str, Any]]] = defaultdict(list)
        for rel in graph.relationships:
            from_type = graph.node_type_by_uuid[rel.from_uuid]
            to_type = graph.node_type_by_uuid[rel.to_uuid]
            doc = {
                **rel.properties,
                "_key": rel.uuid,
                "_from": f"{sanitize_db_identifier(from_type)}/{rel.from_uuid}",
                "_to": f"{sanitize_db_identifier(to_type)}/{rel.to_uuid}",
                "uuid": rel.uuid,
                "type": rel.type,
                "source": rel.source,
            }
            rels_by_type[rel.type].append(doc)

        for rel_type, docs in sorted(rels_by_type.items()):
            collection = sanitize_db_identifier(rel_type)
            for batch in _chunks(docs, self.batch_size):
                response = self.session.post(
                    self.db_api(f"/_api/document/{collection}"),
                    params={"overwriteMode": "replace"},
                    json=list(batch),
                    timeout=900,
                )
                self.check(response, (201, 202), f"Insert Arango edges into {collection}")

        edge_definitions = [
            {"collection": edge_col, "from": vertex_collections, "to": vertex_collections}
            for edge_col in edge_collections
        ]
        response = self.session.post(
            self.db_api("/_api/gharial"),
            json={"name": GRAPH_NAME, "edgeDefinitions": edge_definitions, "orphanCollections": vertex_collections},
            timeout=900,
        )
        self.check(response, (201, 202, 409), "Create Arango named graph")

    def _cursor_query(self, query: str, bind_vars: dict[str, Any]) -> list[Any]:
        response = self.session.post(
            self.db_api("/_api/cursor"),
            json={"query": query, "bindVars": bind_vars, "batchSize": 10000},
            timeout=900,
        )
        payload = self.check(response, (201,), "Run Arango cursor query")
        result = list(payload.get("result", []))
        cursor_id = payload.get("id")
        while payload.get("hasMore") and cursor_id:
            response = self.session.put(self.db_api(f"/_api/cursor/{cursor_id}"), timeout=900)
            payload = self.check(response, (200,), "Read next Arango cursor page")
            result.extend(payload.get("result", []))
            cursor_id = payload.get("id")
        return result

    def bfs(self, graph: GraphData, start_uuid: str, max_depth: int, output_csv: str | Path) -> int:
        try:
            start_id = f"{sanitize_db_identifier(graph.start_type(start_uuid))}/{start_uuid}"
        except ValueError as exc:
            raise ValueError(f"Cannot build Arango start id for {start_uuid}") from exc
        query = f"""
            FOR v, e, p IN 0..@maxDepth ANY @startId GRAPH '{GRAPH_NAME}'
              OPTIONS {{ order: "bfs", uniqueVertices: "global" }}
              RETURN {{ vertex_id: v.uuid, distance: LENGTH(p.edges) }}
        """
        rows = self._cursor_query(query, {"startId": start_id, "maxDepth": max_depth})
        pairs = [(r["vertex_id"], int(r["distance"])) for r in rows]
        write_bfs_csv(output_csv, pairs)
        return len(pairs)
