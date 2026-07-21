from __future__ import annotations

import json
from collections import defaultdict, deque
from pathlib import Path
from typing import Any, DefaultDict, Iterable, Optional, Sequence

import requests

from ..common import sanitize_db_identifier
from ..graph_data import GraphData, NodeRecord, RelationshipRecord
from tools.graphio.id_codec import parse_global_id, to_signed_storage

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


def write_bfs_csv(path: str | Path, rows: Iterable[tuple[int | str, int]]) -> None:
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    ordered = sorted(rows, key=lambda x: (x[1], x[0]))
    with open(out, "w", encoding="utf-8") as f:
        f.write("vertex_id;distance\n")
        for vertex_id, distance in ordered:
            f.write(f"{vertex_id};{distance}\n")


class Neo4jNativeClient:
    """Native full-replica Neo4j loader + GDS BFS query client.

    Real Neo4j entities deliberately carry no logical ``uuid`` property. The
    benchmark keeps the input-global <-> Neo4j-native mapping in this process,
    projects the database into GDS once after loading, and translates GDS native
    IDs back to the benchmark's uint64 IDs when writing CSV output.
    """

    def __init__(self, endpoint: str, user: str, password: str, *, batch_size: int = 10000):
        self.endpoint = endpoint
        self.batch_size = batch_size
        self.session = requests.Session()
        self._graph: GraphData | None = None
        self._native_by_global: dict[int, int] = {}
        self._global_by_native: dict[int, int] = {}
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

    def _drop_gds_projection(self) -> None:
        self.tx([{
            "statement": "CALL gds.graph.drop($graphName, false) YIELD graphName RETURN graphName",
            "parameters": {"graphName": GRAPH_NAME},
        }], timeout=300)

    def reset(self) -> None:
        # A named projection retains the previous native IDs and must be dropped
        # before deleting/reloading the physical graph.
        self._drop_gds_projection()
        self._graph = None
        self._native_by_global.clear()
        self._global_by_native.clear()

        # Delete relationships before nodes. A batch of N DETACH DELETE nodes can
        # still detach hundreds of thousands of relationships when it happens to
        # contain high-degree vertices, which makes the transaction-state size
        # unrelated to N and can exhaust Neo4j's transaction-memory pool.
        relationship_delete_batch_size = max(1_000, min(self.batch_size * 4, 10_000))
        relationship_statement = f"""
            MATCH (n:{_entity_label()})-[r]-()
            WITH DISTINCT r LIMIT $limit
            DELETE r
            RETURN count(*) AS deleted
        """
        while True:
            payload = self.tx([{
                "statement": relationship_statement,
                "parameters": {"limit": relationship_delete_batch_size},
            }], timeout=900)
            rows = payload["results"][0]["data"]
            deleted = int(rows[0]["row"][0]) if rows else 0
            if deleted == 0:
                break

        # All benchmark nodes are isolated now, so plain DELETE has predictable
        # transaction memory and does not trigger implicit relationship deletion.
        node_delete_batch_size = max(1_000, min(self.batch_size * 10, 10_000))
        node_statement = f"""
            MATCH (n:{_entity_label()})
            WITH n LIMIT $limit
            DELETE n
            RETURN count(*) AS deleted
        """
        while True:
            payload = self.tx([{
                "statement": node_statement,
                "parameters": {"limit": node_delete_batch_size},
            }], timeout=900)
            rows = payload["results"][0]["data"]
            deleted = int(rows[0]["row"][0]) if rows else 0
            if deleted == 0:
                break

    def _project_gds_graph(self, relationship_types: Iterable[str]) -> None:
        relationship_projection = {
            relationship_type: {
                "type": relationship_type,
                "orientation": "UNDIRECTED",
            }
            for relationship_type in sorted(set(relationship_types))
        }
        self.tx([{
            "statement": (
                "CALL gds.graph.project($graphName, $nodeLabel, $relationshipProjection) "
                "YIELD graphName, nodeCount, relationshipCount "
                "RETURN graphName, nodeCount, relationshipCount"
            ),
            "parameters": {
                "graphName": GRAPH_NAME,
                "nodeLabel": GRAPH_NODE_LABEL,
                "relationshipProjection": relationship_projection,
            },
        }], timeout=900)

    def load_full_graph(self, graph: GraphData) -> None:
        self.reset()
        self._graph = graph

        nodes_by_type: DefaultDict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in graph.nodes:
            props = {**node.properties, "type": node.type, "source": node.source}
            props.pop("uuid", None)
            nodes_by_type[node.type].append({"input_id": str(node.uuid), "props": props})

        for node_type, rows in sorted(nodes_by_type.items()):
            label = _cypher_ident(node_type)
            statement = f"""
                UNWIND $rows AS row
                CREATE (n:{_entity_label()}:{label})
                SET n = row.props
                RETURN row.input_id AS input_id, id(n) AS native_id
            """
            for batch in _chunks(rows, self.batch_size):
                payload = self.tx([{"statement": statement, "parameters": {"rows": list(batch)}}], timeout=900)
                for item in payload["results"][0]["data"]:
                    input_id, native_id = item["row"]
                    global_id = parse_global_id(input_id)
                    native = int(native_id)
                    self._native_by_global[global_id] = native
                    self._global_by_native[native] = global_id

        if len(self._native_by_global) != len(graph.nodes):
            raise RuntimeError(
                f"Neo4j returned {len(self._native_by_global)} native node IDs for {len(graph.nodes)} nodes"
            )

        rels_by_type: DefaultDict[str, list[dict[str, Any]]] = defaultdict(list)
        for rel in graph.relationships:
            props = {**rel.properties, "type": rel.type, "source": rel.source}
            props.pop("uuid", None)
            rels_by_type[rel.type].append({
                "from_native": self._native_by_global[rel.from_uuid],
                "to_native": self._native_by_global[rel.to_uuid],
                "props": props,
            })

        for rel_type, rows in sorted(rels_by_type.items()):
            rtype = _cypher_ident(rel_type)
            statement = f"""
                UNWIND $rows AS row
                MATCH (a:{_entity_label()}) WHERE id(a) = row.from_native
                MATCH (b:{_entity_label()}) WHERE id(b) = row.to_native
                CREATE (a)-[r:{rtype}]->(b)
                SET r = row.props
            """
            for batch in _chunks(rows, self.batch_size):
                self.tx([{"statement": statement, "parameters": {"rows": list(batch)}}], timeout=900)

        self._project_gds_graph(rels_by_type)

    @staticmethod
    def _reference_distances(
        graph: GraphData,
        start_id: int,
        max_depth: int,
        relationship_types: set[str] | None,
    ) -> dict[int, int]:
        adjacency: DefaultDict[int, list[int]] = defaultdict(list)
        for relationship in graph.relationships:
            if relationship_types and relationship.type not in relationship_types:
                continue
            adjacency[relationship.from_uuid].append(relationship.to_uuid)
            adjacency[relationship.to_uuid].append(relationship.from_uuid)

        distances = {start_id: 0}
        queue = deque([start_id])
        while queue:
            source = queue.popleft()
            depth = distances[source]
            if depth >= max_depth:
                continue
            for neighbor in adjacency.get(source, []):
                if neighbor in distances:
                    continue
                distances[neighbor] = depth + 1
                queue.append(neighbor)
        return distances

    def bfs(self, start_uuid: str, relationship_filter: str, max_depth: int, output_csv: str | Path) -> int:
        if self._graph is None:
            raise RuntimeError("Neo4j graph must be loaded before running GDS BFS")

        start_id = parse_global_id(start_uuid)
        try:
            source_native = self._native_by_global[start_id]
        except KeyError as exc:
            raise ValueError(f"Start ID is not present in loaded Neo4j graph: {start_uuid}") from exc

        relationship_types = {value for value in relationship_filter.split("|") if value}
        configuration: dict[str, Any] = {
            "sourceNode": source_native,
            "maxDepth": int(max_depth),
            "logProgress": False,
        }
        if relationship_types:
            configuration["relationshipTypes"] = sorted(relationship_types)

        payload = self.tx([{
            "statement": (
                "WITH $configuration AS config "
                "CALL gds.bfs.stream("
                "$graphName, "
                "config {"
                ".*, "
                "sourceNode: toInteger(toString(config.sourceNode)), "
                "maxDepth: toInteger(toString(config.maxDepth))"
                "}"
                ") "
                "YIELD sourceNode, nodeIds "
                "RETURN sourceNode, nodeIds"
            ),
            "parameters": {"graphName": GRAPH_NAME, "configuration": configuration},
        }], timeout=900)

        data = payload["results"][0]["data"]
        if len(data) != 1 or len(data[0].get("row", [])) < 2:
            raise RuntimeError(f"Unexpected gds.bfs.stream output: {data}")
        returned_source, native_ids = data[0]["row"]
        if int(returned_source) != source_native or not isinstance(native_ids, list):
            raise RuntimeError(f"Unexpected gds.bfs.stream output row: {data[0]['row']}")

        try:
            visited = {self._global_by_native[int(native_id)] for native_id in native_ids}
        except KeyError as exc:
            raise RuntimeError(f"GDS returned an unknown Neo4j native node ID: {exc.args[0]}") from exc

        # gds.bfs returns the visited native IDs and a traversal-order path, not
        # per-vertex levels. Reconstruct levels from the same in-memory input and
        # assert that the visited set exactly matches GDS before writing the CSV.
        distances = self._reference_distances(
            self._graph, start_id, max_depth, relationship_types or None
        )
        if visited != set(distances):
            missing = sorted(set(distances) - visited)[:10]
            extra = sorted(visited - set(distances))[:10]
            raise RuntimeError(
                "gds.bfs.stream visited-set mismatch "
                f"(missing={missing}, extra={extra}, gds={len(visited)}, expected={len(distances)})"
            )

        write_bfs_csv(output_csv, distances.items())
        return len(distances)


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

    def ensure_uuid_index(self, name: str) -> None:
        collection = sanitize_db_identifier(name)
        response = self.session.post(
            self.db_api(f"/_api/index?collection={collection}"),
            json={
                "type": "persistent",
                "fields": ["uuid"],
                "unique": True,
                "sparse": False,
                "name": sanitize_db_identifier(f"{collection}_uuid_unique"),
            },
            timeout=300,
        )
        if response.status_code not in (200, 201, 409):
            raise RuntimeError(
                f"Create Arango uuid index for {name} failed: HTTP {response.status_code}: {response.text}"
            )

    def load_full_graph(self, graph: GraphData) -> None:
        self.reset_database()
        vertex_collections = sorted({sanitize_db_identifier(n.type) for n in graph.nodes})
        edge_collections = sorted({sanitize_db_identifier(r.type) for r in graph.relationships})
        for collection in vertex_collections:
            self.ensure_collection(collection, edge=False)
            self.ensure_uuid_index(collection)
        for collection in edge_collections:
            self.ensure_collection(collection, edge=True)
            self.ensure_uuid_index(collection)

        nodes_by_type: DefaultDict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in graph.nodes:
            doc = {**node.properties, "_key": str(node.uuid), "uuid": to_signed_storage(node.uuid), "type": node.type, "source": node.source}
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
                "_key": str(rel.uuid),
                "_from": f"{sanitize_db_identifier(from_type)}/{rel.from_uuid}",
                "_to": f"{sanitize_db_identifier(to_type)}/{rel.to_uuid}",
                "uuid": to_signed_storage(rel.uuid),
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
            json={"name": GRAPH_NAME, "edgeDefinitions": edge_definitions, "orphanCollections": []},
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
            normalized_start = parse_global_id(start_uuid)
            start_id = f"{sanitize_db_identifier(graph.start_type(normalized_start))}/{normalized_start}"
        except ValueError as exc:
            raise ValueError(f"Cannot build Arango start id for {start_uuid}") from exc
        query = f"""
            FOR v, e, p IN 0..@maxDepth ANY @startId GRAPH '{GRAPH_NAME}'
              OPTIONS {{ order: "bfs", uniqueVertices: "global" }}
              RETURN {{ vertex_id: v.uuid, distance: LENGTH(p.edges) }}
        """
        rows = self._cursor_query(query, {"startId": start_id, "maxDepth": max_depth})
        pairs = [(parse_global_id(r["vertex_id"]), int(r["distance"])) for r in rows]
        write_bfs_csv(output_csv, pairs)
        return len(pairs)
