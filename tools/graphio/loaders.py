from __future__ import annotations

import base64
import http.client
import json
import re
import socket
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from dataclasses import dataclass
from itertools import islice
from pathlib import Path
from typing import Any, Iterable, Iterator, TypeVar

from .domain import sanitize_name
from .id_codec import BackendLayout
from .model import NodeRecord, RelationshipRecord
from .reader import open_graph_reader

T = TypeVar("T")
PROXY_NODE_TYPE = "Proxy"


@dataclass(frozen=True)
class EndpointInfo:
    type: str
    source: str


@dataclass(frozen=True)
class BackendConnection:
    source: str
    endpoint: str
    database: str
    username: str
    password: str


def batched(values: Iterable[T], size: int) -> Iterator[list[T]]:
    if size <= 0:
        raise ValueError("batch size must be positive")
    iterator = iter(values)
    while batch := list(islice(iterator, size)):
        yield batch


def _request_json(
    method: str,
    url: str,
    *,
    payload: Any | None = None,
    auth: tuple[str, str] | None = None,
    expected: tuple[int, ...] = (200,),
    timeout: int = 300,
    content_type: str = "application/json",
    attempts: int = 8,
    retry_delay: float = 2.0,
) -> Any:
    data = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    transient_http_codes = {429, 502, 503, 504}
    transient_network_errors = (
        ConnectionAbortedError,
        ConnectionRefusedError,
        ConnectionResetError,
        BrokenPipeError,
        TimeoutError,
        socket.timeout,
        http.client.RemoteDisconnected,
        urllib.error.URLError,
    )
    last_error: BaseException | None = None

    for attempt in range(1, attempts + 1):
        request = urllib.request.Request(url, data=data, method=method)
        request.add_header("Accept", "application/json")
        if data is not None:
            request.add_header("Content-Type", content_type)
        if auth:
            token = base64.b64encode(f"{auth[0]}:{auth[1]}".encode("utf-8")).decode("ascii")
            request.add_header("Authorization", f"Basic {token}")

        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                body = response.read().decode("utf-8")
                status = response.status
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace")
            if exc.code in expected:
                return json.loads(body) if body else {}
            if exc.code not in transient_http_codes or attempt == attempts:
                raise RuntimeError(f"{method} {url}: HTTP {exc.code}: {body}") from exc
            last_error = exc
        except transient_network_errors as exc:
            if attempt == attempts:
                reason = exc.reason if isinstance(exc, urllib.error.URLError) else str(exc)
                raise RuntimeError(
                    f"{method} {url} failed after {attempts} attempts: {reason}"
                ) from exc
            last_error = exc
        else:
            if status not in expected:
                raise RuntimeError(f"{method} {url}: HTTP {status}: {body}")
            return json.loads(body) if body else {}

        time.sleep(retry_delay * min(attempt, 5))

    raise RuntimeError(f"{method} {url} failed: {last_error}")


def read_backend_connections(config_path: str | Path) -> dict[str, BackendConnection]:
    raw = json.loads(Path(config_path).read_text(encoding="utf-8"))
    result: dict[str, BackendConnection] = {}
    for entry in raw.get("backends", []):
        if not entry.get("enabled", True):
            continue
        source = str(entry.get("id", ""))
        if source not in {"Neo4J", "ArangoDB"}:
            continue
        connection = entry.get("connection", {})
        credentials = connection.get("credentials", {})
        result[source] = BackendConnection(
            source=source,
            endpoint=str(connection.get("endpoint", "")),
            database=str(connection.get("database", "")),
            username=str(credentials.get("username", "")),
            password=str(credentials.get("password", "")),
        )
    return result


def read_enabled_backend_names(config_path: str | Path) -> set[str]:
    raw = json.loads(Path(config_path).read_text(encoding="utf-8"))
    return {
        str(entry.get("id", ""))
        for entry in raw.get("backends", [])
        if entry.get("enabled", True)
    }


def read_uuid_routing_layout(config_path: str | Path) -> BackendLayout:
    raw = json.loads(Path(config_path).read_text(encoding="utf-8"))
    configured = raw.get("uuidRouting")
    if not configured:
        raise ValueError("Backend config must declare a non-empty uuidRouting array")
    layout = BackendLayout(tuple(str(value) for value in configured))
    enabled = BackendLayout.from_sources(
        str(entry.get("id", ""))
        for entry in raw.get("backends", [])
        if entry.get("enabled", True)
    )
    disabled_routes = set(layout.backends) - set(enabled.backends)
    if disabled_routes:
        raise ValueError(f"UUID routing backends are not enabled: {sorted(disabled_routes)}")
    return layout


def validate_graph_routing_layout(input_graph: str | Path, routing_layout: BackendLayout) -> None:
    metadata = open_graph_reader(input_graph).metadata()
    configured = metadata.get("uuidRouting")
    if configured is None:
        raise ValueError(
            "Graph has no uuidRouting metadata; regenerate or migrate UUIDs before loading"
        )
    graph_layout = BackendLayout(tuple(str(value) for value in configured))
    if graph_layout != routing_layout:
        raise ValueError(
            f"Graph UUID routing layout {graph_layout.backends} does not match backend config "
            f"{routing_layout.backends}; UUID suffixes would be decoded incorrectly"
        )
    declared_bits = metadata.get("uuidRoutingBits")
    if declared_bits is not None and int(declared_bits) != routing_layout.bit_count:
        raise ValueError(
            f"Graph declares uuidRoutingBits={declared_bits}, expected {routing_layout.bit_count}"
        )


def build_endpoint_directory(
    input_graph: str | Path,
    routing_layout: BackendLayout,
) -> dict[str, EndpointInfo]:
    directory: dict[str, EndpointInfo] = {}
    for node in open_graph_reader(input_graph).iter_nodes():
        if node.uuid in directory:
            raise ValueError(f"Duplicate node UUID: {node.uuid}")
        directory[node.uuid] = EndpointInfo(
            type=node.type,
            source=routing_layout.decode_source(node.uuid),
        )
    return directory


def _build_proxy_node(local_source: str, uuid: str, remote: EndpointInfo) -> NodeRecord:
    return NodeRecord(
        uuid=uuid,
        type=PROXY_NODE_TYPE,
        source=local_source,
        properties={"is_proxy": True, "remote-type": remote.type, "remote-source": remote.source},
    )


class Neo4jHttpAdapter:
    def __init__(self, connection: BackendConnection):
        self.source_name = "Neo4J"
        self.connection = connection
        self.auth = (connection.username, connection.password)

    def _cypher(self, statement: str, parameters: dict[str, Any] | None = None) -> dict[str, Any]:
        response = _request_json(
            "POST",
            self.connection.endpoint,
            payload={"statements": [{"statement": statement, "parameters": parameters or {}}]},
            auth=self.auth,
            expected=(200, 201),
            timeout=900,
        )
        errors = response.get("errors", [])
        if errors:
            raise RuntimeError(f"Neo4j query failed: {errors}")
        return response

    def reset(self) -> None:
        # Deleting a large graph in one transaction can exhaust Neo4j's
        # transaction memory pool. Delete a bounded number of nodes per
        # committed HTTP transaction instead.
        delete_batch_size = 1_000
        while True:
            response = self._cypher(
                "MATCH (n) "
                "WITH n LIMIT $limit "
                "DETACH DELETE n "
                "RETURN count(*) AS deleted",
                {"limit": delete_batch_size},
            )
            results = response.get("results", [])
            data = results[0].get("data", []) if results else []
            deleted = int(data[0]["row"][0]) if data else 0
            if deleted == 0:
                break

    def prepare(self, node_types: Iterable[str], _relationship_types: Iterable[str]) -> None:
        for type_name in sorted(set(node_types)):
            label = sanitize_name(type_name)
            constraint = sanitize_name(f"{label.lower()}_uuid_unique")
            self._cypher(
                f"CREATE CONSTRAINT {constraint} IF NOT EXISTS "
                f"FOR (n:{label}) REQUIRE n.uuid IS UNIQUE"
            )

    def insert_nodes(self, nodes: list[NodeRecord]) -> None:
        grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in nodes:
            grouped[node.type].append(
                {
                    "uuid": node.uuid,
                    "props": {"uuid": node.uuid, "type": node.type, "source": node.source, **node.properties},
                }
            )
        for type_name, rows in sorted(grouped.items()):
            label = sanitize_name(type_name)
            self._cypher(
                f"UNWIND $rows AS row MERGE (n:{label} {{uuid: row.uuid}}) SET n += row.props",
                {"rows": rows},
            )

    def insert_relationships(
        self,
        relationships: list[RelationshipRecord],
        local_endpoint_types: dict[str, str],
    ) -> None:
        grouped: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
        for relationship in relationships:
            grouped[
                (
                    relationship.type,
                    local_endpoint_types[relationship.from_uuid],
                    local_endpoint_types[relationship.to_uuid],
                )
            ].append(
                {
                    "uuid": relationship.uuid,
                    "from_uuid": relationship.from_uuid,
                    "to_uuid": relationship.to_uuid,
                    "props": {
                        "uuid": relationship.uuid,
                        "type": relationship.type,
                        "source": relationship.source,
                        **relationship.properties,
                    },
                }
            )
        for (rel_type, from_type, to_type), rows in sorted(grouped.items()):
            self._cypher(
                "\n".join(
                    [
                        "UNWIND $rows AS row",
                        f"MATCH (a:{sanitize_name(from_type)} {{uuid: row.from_uuid}})",
                        f"MATCH (b:{sanitize_name(to_type)} {{uuid: row.to_uuid}})",
                        f"MERGE (a)-[r:{sanitize_name(rel_type)} {{uuid: row.uuid}}]->(b)",
                        "SET r += row.props",
                    ]
                ),
                {"rows": rows},
            )


class ArangoHttpAdapter:
    def __init__(self, connection: BackendConnection):
        self.source_name = "ArangoDB"
        self.connection = connection
        self.auth = (connection.username, connection.password)
        marker = "/_db/"
        if marker in connection.endpoint:
            self.base_url = connection.endpoint.split(marker, 1)[0].rstrip("/")
        else:
            parsed = urllib.parse.urlsplit(connection.endpoint)
            self.base_url = f"{parsed.scheme}://{parsed.netloc}"
        self.database = connection.database or "university"

    def system_api(self, path: str) -> str:
        return f"{self.base_url}{path}"

    def db_api(self, path: str) -> str:
        return f"{self.base_url}/_db/{urllib.parse.quote(self.database, safe='')}{path}"

    def ensure_database(self) -> None:
        payload = _request_json("GET", self.system_api("/_api/database"), auth=self.auth, expected=(200,))
        if self.database not in payload.get("result", []):
            _request_json(
                "POST",
                self.system_api("/_api/database"),
                payload={"name": self.database},
                auth=self.auth,
                expected=(201, 202),
            )

    def reset(self) -> None:
        _request_json(
            "DELETE",
            self.system_api(f"/_api/database/{urllib.parse.quote(self.database, safe='')}"),
            auth=self.auth,
            expected=(200, 202, 404),
        )
        _request_json(
            "POST",
            self.system_api("/_api/database"),
            payload={"name": self.database},
            auth=self.auth,
            expected=(201, 202),
        )

    def ensure_collection(self, name: str, *, edge: bool) -> None:
        _request_json(
            "POST",
            self.db_api("/_api/collection"),
            payload={"name": sanitize_name(name), "type": 3 if edge else 2},
            auth=self.auth,
            expected=(200, 201, 202, 409),
        )

    def prepare(self, node_types: Iterable[str], relationship_types: Iterable[str]) -> None:
        self.ensure_database()
        for name in sorted(set(node_types)):
            self.ensure_collection(name, edge=False)
        for name in sorted(set(relationship_types)):
            self.ensure_collection(name, edge=True)

    def _import_batch(self, collection: str, documents: list[dict[str, Any]]) -> None:
        query = urllib.parse.urlencode(
            {
                "collection": sanitize_name(collection),
                "type": "array",
                "onDuplicate": "replace",
                "complete": "true",
            }
        )
        payload = _request_json(
            "POST",
            f"{self.db_api('/_api/import')}?{query}",
            payload=documents,
            auth=self.auth,
            expected=(201,),
            timeout=900,
        )
        if payload.get("errors", 0):
            raise RuntimeError(f"Arango import into {collection} reported errors: {payload}")

    def insert_nodes(self, nodes: list[NodeRecord]) -> None:
        grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in nodes:
            grouped[node.type].append(
                {
                    "_key": node.uuid,
                    "uuid": node.uuid,
                    "type": node.type,
                    "source": node.source,
                    **node.properties,
                }
            )
        for type_name, documents in sorted(grouped.items()):
            self._import_batch(type_name, documents)

    def insert_relationships(
        self,
        relationships: list[RelationshipRecord],
        local_endpoint_types: dict[str, str],
    ) -> None:
        grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for relationship in relationships:
            grouped[relationship.type].append(
                {
                    "_key": relationship.uuid,
                    "_from": f"{sanitize_name(local_endpoint_types[relationship.from_uuid])}/{relationship.from_uuid}",
                    "_to": f"{sanitize_name(local_endpoint_types[relationship.to_uuid])}/{relationship.to_uuid}",
                    "uuid": relationship.uuid,
                    "type": relationship.type,
                    "source": relationship.source,
                    **relationship.properties,
                }
            )
        for type_name, documents in sorted(grouped.items()):
            self._import_batch(type_name, documents)


def relationship_stored_on(
    relationship: RelationshipRecord,
    source: str,
    endpoint_directory: dict[str, EndpointInfo],
    routing_layout: BackendLayout,
) -> bool:
    """Store the primary edge plus an adjacency replica at both endpoint homes."""
    primary = routing_layout.decode_source(relationship.uuid)
    from_endpoint = endpoint_directory.get(relationship.from_uuid)
    to_endpoint = endpoint_directory.get(relationship.to_uuid)
    if from_endpoint is None or to_endpoint is None:
        missing = relationship.from_uuid if from_endpoint is None else relationship.to_uuid
        raise ValueError(f"Relationship {relationship.uuid} references missing endpoint {missing}")
    return source in {primary, from_endpoint.source, to_endpoint.source}


def inspect_source(
    input_graph: str | Path,
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
                raise ValueError(f"Relationship {relationship.uuid} references missing endpoint {endpoint_uuid}")
            if endpoint.source != source:
                proxy_ids.add(endpoint_uuid)
                local_endpoint_types[endpoint_uuid] = PROXY_NODE_TYPE

    if proxy_ids:
        node_types.add(PROXY_NODE_TYPE)
    return node_types, relationship_types, proxy_ids, local_endpoint_types


def load_source(
    *,
    input_graph: str | Path,
    source: str,
    adapter: Neo4jHttpAdapter | ArangoHttpAdapter,
    endpoint_directory: dict[str, EndpointInfo],
    routing_layout: BackendLayout,
    reset: bool,
    batch_size: int,
) -> tuple[int, int, int]:
    node_types, relationship_types, proxy_ids, local_endpoint_types = inspect_source(
        input_graph, source, endpoint_directory, routing_layout
    )
    if not node_types and not relationship_types:
        return 0, 0, 0

    if reset:
        adapter.reset()
    adapter.prepare(node_types, relationship_types)

    node_count = 0
    local_nodes = (
        node
        for node in open_graph_reader(input_graph).iter_nodes()
        if routing_layout.decode_source(node.uuid) == source
    )
    for batch in batched(local_nodes, batch_size):
        adapter.insert_nodes(batch)
        node_count += len(batch)

    proxy_count = 0
    proxy_nodes = (_build_proxy_node(source, uuid, endpoint_directory[uuid]) for uuid in sorted(proxy_ids))
    for batch in batched(proxy_nodes, batch_size):
        adapter.insert_nodes(batch)
        proxy_count += len(batch)

    relationship_count = 0
    local_relationships = (
        relationship
        for relationship in open_graph_reader(input_graph).iter_relationships()
        if relationship_stored_on(relationship, source, endpoint_directory, routing_layout)
    )
    for batch in batched(local_relationships, batch_size):
        adapter.insert_relationships(batch, local_endpoint_types)
        relationship_count += len(batch)

    return node_count, proxy_count, relationship_count


def inspect_gpu_projection(
    input_graph: str | Path,
    endpoint_directory: dict[str, EndpointInfo],
    routing_layout: BackendLayout,
) -> dict[str, int]:
    """Count records that the C++ runtime materializes into the cuGraph projection."""
    gpu_nodes = sum(1 for endpoint in endpoint_directory.values() if endpoint.source == "GPU")
    gpu_edges = 0
    projection_vertices = {
        uuid for uuid, endpoint in endpoint_directory.items() if endpoint.source == "GPU"
    }
    for relationship in open_graph_reader(input_graph).iter_relationships():
        primary = routing_layout.decode_source(relationship.uuid)
        from_endpoint = endpoint_directory.get(relationship.from_uuid)
        to_endpoint = endpoint_directory.get(relationship.to_uuid)
        if from_endpoint is None or to_endpoint is None:
            missing = relationship.from_uuid if from_endpoint is None else relationship.to_uuid
            raise ValueError(f"Relationship {relationship.uuid} references missing endpoint {missing}")
        if primary == "GPU" or from_endpoint.source == "GPU" or to_endpoint.source == "GPU":
            gpu_edges += 1
            projection_vertices.add(relationship.from_uuid)
            projection_vertices.add(relationship.to_uuid)
    return {
        "nodes": gpu_nodes,
        "proxies": len(projection_vertices) - gpu_nodes,
        "relationships": gpu_edges,
        "projection_vertices": len(projection_vertices),
        "runtime_projection": 1,
    }


def load_graph(
    input_graph: str | Path,
    backend_config: str | Path,
    *,
    reset: bool = False,
    batch_size: int = 5000,
    only_sources: Iterable[str] | None = None,
) -> dict[str, dict[str, int]]:
    if batch_size <= 0:
        raise ValueError("batch_size must be positive")
    connections = read_backend_connections(backend_config)
    enabled_backends = read_enabled_backend_names(backend_config)
    routing_layout = read_uuid_routing_layout(backend_config)
    validate_graph_routing_layout(input_graph, routing_layout)
    selected = set(only_sources or enabled_backends)
    unknown = selected - enabled_backends
    if unknown:
        raise ValueError(f"Selected backends are not enabled/configured: {sorted(unknown)}")

    endpoint_directory = build_endpoint_directory(input_graph, routing_layout)
    result: dict[str, dict[str, int]] = {}
    for source in ("Neo4J", "ArangoDB"):
        if source not in selected:
            continue
        connection = connections[source]
        adapter: Neo4jHttpAdapter | ArangoHttpAdapter
        adapter = Neo4jHttpAdapter(connection) if source == "Neo4J" else ArangoHttpAdapter(connection)
        nodes, proxies, relationships = load_source(
            input_graph=input_graph,
            source=source,
            adapter=adapter,
            endpoint_directory=endpoint_directory,
            routing_layout=routing_layout,
            reset=reset,
            batch_size=batch_size,
        )
        result[source] = {"nodes": nodes, "proxies": proxies, "relationships": relationships}
    if "GPU" in selected:
        result["GPU"] = inspect_gpu_projection(input_graph, endpoint_directory, routing_layout)
    return result
