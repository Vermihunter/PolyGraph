from __future__ import annotations

import base64
import http.client
import json
import re
import socket
import time
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from dataclasses import dataclass
from itertools import islice
from pathlib import Path
from typing import Any, Iterable, Iterator, TypeVar

from .domain import sanitize_name
from .id_codec import (
    BackendLayout,
    parse_global_id,
    routing_names,
    to_signed_storage,
    write_native_id_map,
)
from .model import NodeRecord, RelationshipRecord
from .reader import open_graph_reader
from .writer import GraphJsonLinesWriter

T = TypeVar("T")
PROXY_NODE_TYPE = "Proxy"
PROGRESS_INTERVAL_SECONDS = 5.0


@dataclass(frozen=True)
class EndpointInfo:
    global_id: int
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


def _progress(source: str, message: str) -> None:
    print(f"[cpgdb][load][{source}] {message}", flush=True)


def progress_batched(
    values: Iterable[T],
    size: int,
    *,
    source: str,
    stage: str,
) -> Iterator[list[T]]:
    """Yield batches and report completed work without an extra counting pass."""
    started = time.monotonic()
    last_report = started
    completed = 0
    batch_count = 0
    _progress(source, f"{stage}: started (batch size {size:,})")
    for batch in batched(values, size):
        yield batch
        completed += len(batch)
        batch_count += 1
        now = time.monotonic()
        if now - last_report >= PROGRESS_INTERVAL_SECONDS:
            elapsed = max(now - started, 1e-9)
            _progress(
                source,
                f"{stage}: {completed:,} records in {batch_count:,} batches "
                f"({completed / elapsed:,.0f} records/s, {elapsed:,.1f}s elapsed)",
            )
            last_report = now
    elapsed = time.monotonic() - started
    _progress(
        source,
        f"{stage}: completed {completed:,} records in {batch_count:,} batches "
        f"({elapsed:,.1f}s)",
    )


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


def read_id_routing_layout(config_path: str | Path) -> BackendLayout:
    raw = json.loads(Path(config_path).read_text(encoding="utf-8"))
    configured = routing_names(raw)
    if not configured:
        raise ValueError("Backend config must declare a non-empty idRouting array")
    layout = BackendLayout(tuple(configured))
    enabled = BackendLayout.from_sources(
        str(entry.get("id", ""))
        for entry in raw.get("backends", [])
        if entry.get("enabled", True)
    )
    disabled_routes = set(layout.backends) - set(enabled.backends)
    if disabled_routes:
        raise ValueError(f"ID routing backends are not enabled: {sorted(disabled_routes)}")
    return layout


read_uuid_routing_layout = read_id_routing_layout


def validate_graph_routing_layout(input_graph: str | Path, routing_layout: BackendLayout) -> None:
    metadata = open_graph_reader(input_graph).metadata()
    configured = routing_names(metadata)
    if configured is None:
        raise ValueError(
            "Graph has no idRouting metadata; regenerate or migrate IDs before loading"
        )
    graph_layout = BackendLayout(tuple(configured))
    if graph_layout != routing_layout:
        raise ValueError(
            f"Graph ID routing layout {graph_layout.backends} does not match backend config "
            f"{routing_layout.backends}; MSB routing bits would be decoded incorrectly"
        )
    declared_bits = metadata.get("idRoutingBits")
    if declared_bits is not None and int(declared_bits) != routing_layout.bit_count:
        raise ValueError(
            f"Graph declares idRoutingBits={declared_bits}, expected {routing_layout.bit_count}"
        )


def build_endpoint_directory(
    input_graph: str | Path,
    routing_layout: BackendLayout,
    *,
    progress_source: str | None = None,
) -> dict[int, EndpointInfo]:
    directory: dict[int, EndpointInfo] = {}
    started = time.monotonic()
    last_report = started
    for node in open_graph_reader(input_graph).iter_nodes():
        if node.uuid in directory:
            raise ValueError(f"Duplicate node ID: {node.uuid}")
        directory[node.uuid] = EndpointInfo(
            global_id=node.uuid,
            type=node.type,
            source=routing_layout.decode_source(node.uuid),
        )
        now = time.monotonic()
        if progress_source and now - last_report >= PROGRESS_INTERVAL_SECONDS:
            _progress(
                progress_source,
                f"endpoint directory: indexed {len(directory):,} nodes "
                f"({now - started:,.1f}s elapsed)",
            )
            last_report = now
    if progress_source:
        _progress(
            progress_source,
            f"endpoint directory: completed {len(directory):,} nodes "
            f"({time.monotonic() - started:,.1f}s)",
        )
    return directory


def _build_proxy_node(local_source: str, uuid: int, remote: EndpointInfo) -> NodeRecord:
    return NodeRecord(
        uuid=uuid,
        type=PROXY_NODE_TYPE,
        source=local_source,
        properties={"is_proxy": True, "remote-type": remote.type, "remote-source": remote.source},
    )


class Neo4jQueryError(RuntimeError):
    def __init__(self, errors: list[dict[str, Any]]):
        self.errors = errors
        super().__init__(f"Neo4j query failed: {errors}")

    def is_memory_pool_oom(self) -> bool:
        return any(
            str(error.get("code", "")).endswith("MemoryPoolOutOfMemoryError")
            for error in self.errors
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
            raise Neo4jQueryError(errors)
        return response

    @staticmethod
    def _deleted_count(response: dict[str, Any]) -> int:
        results = response.get("results", [])
        data = results[0].get("data", []) if results else []
        return int(data[0]["row"][0]) if data else 0

    def _delete_in_batches(self, statement: str, *, batch_size: int) -> None:
        current_batch_size = batch_size
        while True:
            try:
                response = self._cypher(statement, {"limit": current_batch_size})
            except Neo4jQueryError as error:
                if not error.is_memory_pool_oom() or current_batch_size == 1:
                    raise
                # Dense graphs can make even a modest fixed batch unexpectedly
                # expensive. Retry the failed transaction with a smaller batch.
                current_batch_size = max(1, current_batch_size // 2)
                continue

            if self._deleted_count(response) == 0:
                return

    def reset(self, *, batch_size: int = 1_000) -> None:
        if batch_size <= 0:
            raise ValueError("reset batch size must be positive")

        # DETACH DELETE on a batch of high-degree nodes may still touch millions
        # of relationships in one transaction. Remove relationships separately
        # first, so node deletion has small and predictable transaction state.
        delete_batch_size = min(batch_size, 1_000)
        self._delete_in_batches(
            "MATCH ()-[r]->() "
            "WITH r LIMIT $limit "
            "DELETE r "
            "RETURN count(r) AS deleted",
            batch_size=delete_batch_size,
        )
        self._delete_in_batches(
            "MATCH (n) "
            "WITH n LIMIT $limit "
            "DELETE n "
            "RETURN count(n) AS deleted",
            batch_size=delete_batch_size,
        )

    @staticmethod
    def _rows(response: dict[str, Any]) -> list[list[Any]]:
        results = response.get("results", [])
        data = results[0].get("data", []) if results else []
        return [list(item.get("row", [])) for item in data]

    def prepare(self, node_types: Iterable[str], _relationship_types: Iterable[str]) -> None:
        del node_types
        # Only proxy vertices carry a logical global ID property. Real Neo4j
        # vertices are addressed by id(n), so no per-label uuid indexes are built.
        self._cypher(
            "CREATE CONSTRAINT proxy_uuid_unique IF NOT EXISTS "
            "FOR (n:Proxy) REQUIRE n.uuid IS UNIQUE"
        )

    def insert_native_nodes(
        self,
        nodes: list[NodeRecord],
        routing_layout: BackendLayout,
    ) -> dict[int, int]:
        grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in nodes:
            props = dict(node.properties)
            props.pop("uuid", None)
            grouped[node.type].append(
                {
                    "input_id": str(node.uuid),
                    "type": node.type,
                    "source": self.source_name,
                    "props": props,
                }
            )

        remapped: dict[int, int] = {}
        for type_name, rows in sorted(grouped.items()):
            label = sanitize_name(type_name)
            response = self._cypher(
                " ".join(
                    [
                        "UNWIND $rows AS row",
                        f"CREATE (n:{label}:CPGDBVertex:__GraphNode)",
                        "SET n = row.props, n.type = row.type, n.source = row.source",
                        "RETURN row.input_id AS input_id, id(n) AS native_id",
                    ]
                ),
                {"rows": rows},
            )
            for input_id, native_id in self._rows(response):
                remapped[parse_global_id(input_id)] = routing_layout.encode_local(
                    int(native_id), self.source_name
                )
        return remapped

    def insert_proxy_nodes(self, nodes: list[NodeRecord]) -> None:
        rows = []
        for node in nodes:
            props = dict(node.properties)
            props.pop("uuid", None)
            rows.append(
                {
                    "uuid": to_signed_storage(node.uuid),
                    "type": node.type,
                    "source": self.source_name,
                    "props": props,
                }
            )
        if not rows:
            return
        self._cypher(
            "UNWIND $rows AS row "
            "MERGE (n:Proxy:CPGDBVertex:__GraphNode {uuid: row.uuid}) "
            "SET n += row.props, n.type = row.type, n.source = row.source",
            {"rows": rows},
        )

    def insert_nodes(self, nodes: list[NodeRecord]) -> None:
        # Compatibility path used by tests/older callers: these are proxies.
        self.insert_proxy_nodes(nodes)

    def insert_relationships(
        self,
        relationships: list[RelationshipRecord],
        local_endpoint_types: dict[int, str],
        endpoint_directory: dict[int, EndpointInfo],
        routing_layout: BackendLayout,
    ) -> dict[int, int]:
        del local_endpoint_types  # Native IDs and Proxy.uuid uniquely identify endpoints.
        grouped: dict[tuple[str, bool, bool, bool], list[dict[str, Any]]] = defaultdict(list)
        for relationship in relationships:
            from_endpoint = endpoint_directory[relationship.from_uuid]
            to_endpoint = endpoint_directory[relationship.to_uuid]
            primary_is_native = routing_layout.decode_source(relationship.uuid) == self.source_name
            from_proxy = from_endpoint.source != self.source_name
            to_proxy = to_endpoint.source != self.source_name
            grouped[
                (
                    relationship.type,
                    primary_is_native,
                    from_proxy,
                    to_proxy,
                )
            ].append(
                {
                    "input_id": str(relationship.uuid),
                    "stored_id": to_signed_storage(relationship.uuid),
                    "from_id": (
                        routing_layout.local_id(relationship.from_uuid)
                        if from_endpoint.source == self.source_name
                        else to_signed_storage(relationship.from_uuid)
                    ),
                    "to_id": (
                        routing_layout.local_id(relationship.to_uuid)
                        if to_endpoint.source == self.source_name
                        else to_signed_storage(relationship.to_uuid)
                    ),
                    "props": {
                        "type": relationship.type,
                        "source": relationship.source,
                        **{k: v for k, v in relationship.properties.items() if k != "uuid"},
                    },
                }
            )

        remapped: dict[int, int] = {}
        for (
            rel_type,
            primary_is_native,
            from_proxy,
            to_proxy,
        ), rows in sorted(grouped.items()):
            uuid_assignment = "REMOVE r.uuid" if primary_is_native else "SET r.uuid = row.stored_id"
            from_match = (
                "MATCH (a:Proxy {uuid: row.from_id})"
                if from_proxy
                else "MATCH (a) WHERE id(a) = row.from_id"
            )
            to_match = (
                "MATCH (b:Proxy {uuid: row.to_id})"
                if to_proxy
                else "MATCH (b) WHERE id(b) = row.to_id"
            )
            response = self._cypher(
                "\n".join(
                    [
                        "UNWIND $rows AS row",
                        from_match,
                        to_match,
                        f"CREATE (a)-[r:{sanitize_name(rel_type)}]->(b)",
                        "SET r = row.props",
                        uuid_assignment,
                        "RETURN row.input_id AS input_id, id(r) AS native_id",
                    ]
                ),
                {"rows": rows},
            )
            if primary_is_native:
                for input_id, native_id in self._rows(response):
                    remapped[parse_global_id(input_id)] = routing_layout.encode_local(
                        int(native_id), self.source_name
                    )
        return remapped


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

    def reset(self, *, batch_size: int = 1_000) -> None:
        del batch_size  # ArangoDB resets by recreating the whole database.
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

    def ensure_uuid_index(self, name: str) -> None:
        collection = sanitize_name(name)
        _request_json(
            "POST",
            self.db_api(f"/_api/index?collection={urllib.parse.quote(collection, safe='')}"),
            payload={
                "type": "persistent",
                "fields": ["uuid"],
                "unique": True,
                "sparse": False,
                "name": sanitize_name(f"{collection}_uuid_unique"),
            },
            auth=self.auth,
            expected=(200, 201, 202, 409),
        )

    def prepare(self, node_types: Iterable[str], relationship_types: Iterable[str]) -> None:
        self.ensure_database()
        for name in sorted(set(node_types)):
            self.ensure_collection(name, edge=False)
        for name in sorted(set(relationship_types)):
            self.ensure_collection(name, edge=True)

    def finalize(self, node_types: Iterable[str], relationship_types: Iterable[str]) -> None:
        # Building persistent indexes after bulk import is substantially faster
        # than maintaining them document-by-document during every import batch.
        for name in sorted(set(node_types)):
            self.ensure_uuid_index(name)
        for name in sorted(set(relationship_types)):
            self.ensure_uuid_index(name)

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
                    **node.properties,
                    "_key": str(node.uuid),
                    "uuid": to_signed_storage(node.uuid),
                    "type": node.type,
                    "source": node.source,
                }
            )
        for type_name, documents in sorted(grouped.items()):
            self._import_batch(type_name, documents)

    def insert_relationships(
        self,
        relationships: list[RelationshipRecord],
        local_endpoint_types: dict[int, str],
    ) -> None:
        grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for relationship in relationships:
            grouped[relationship.type].append(
                {
                    **relationship.properties,
                    "_key": str(relationship.uuid),
                    "_from": f"{sanitize_name(local_endpoint_types[relationship.from_uuid])}/{relationship.from_uuid}",
                    "_to": f"{sanitize_name(local_endpoint_types[relationship.to_uuid])}/{relationship.to_uuid}",
                    "uuid": to_signed_storage(relationship.uuid),
                    "type": relationship.type,
                    "source": relationship.source,
                }
            )
        for type_name, documents in sorted(grouped.items()):
            self._import_batch(type_name, documents)


def relationship_stored_on(
    relationship: RelationshipRecord,
    source: str,
    endpoint_directory: dict[int, EndpointInfo],
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
    endpoint_directory: dict[int, EndpointInfo],
    routing_layout: BackendLayout,
    *,
    progress_source: str | None = None,
) -> tuple[set[str], set[str], set[int], dict[int, str]]:
    node_types = {endpoint.type for endpoint in endpoint_directory.values() if endpoint.source == source}
    relationship_types: set[str] = set()
    proxy_ids: set[int] = set()
    local_endpoint_types: dict[int, str] = {
        uuid: endpoint.type for uuid, endpoint in endpoint_directory.items() if endpoint.source == source
    }

    inspected = 0
    started = time.monotonic()
    last_report = started
    for relationship in open_graph_reader(input_graph).iter_relationships():
        inspected += 1
        if relationship_stored_on(relationship, source, endpoint_directory, routing_layout):
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

        now = time.monotonic()
        if progress_source and now - last_report >= PROGRESS_INTERVAL_SECONDS:
            _progress(
                progress_source,
                f"placement scan: inspected {inspected:,} relationships; "
                f"selected {len(relationship_types):,} relationship types and "
                f"{len(proxy_ids):,} proxies ({now - started:,.1f}s elapsed)",
            )
            last_report = now

    if proxy_ids:
        node_types.add(PROXY_NODE_TYPE)
    if progress_source:
        _progress(
            progress_source,
            f"placement scan: completed {inspected:,} relationships, "
            f"{len(proxy_ids):,} proxies ({time.monotonic() - started:,.1f}s)",
        )
    return node_types, relationship_types, proxy_ids, local_endpoint_types


def _rewrite_graph_ids(
    input_graph: str | Path,
    output_graph: str | Path,
    routing_layout: BackendLayout,
    *,
    node_mapping: dict[int, int] | None = None,
    relationship_mapping: dict[int, int] | None = None,
    neo4j_resolved: bool = False,
    progress_source: str | None = None,
    stage: str = "rewrite graph IDs",
) -> Path:
    node_mapping = node_mapping or {}
    relationship_mapping = relationship_mapping or {}
    source = Path(input_graph).expanduser().resolve()
    output = Path(output_graph).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    metadata = dict(open_graph_reader(source).metadata())
    strategy = metadata.pop("idRoutingStrategy", "uint64-msb-v1")
    for key in ("idRouting", "idRoutingBits", "idRoutingPosition", "idType", "uuidType"):
        metadata.pop(key, None)
    metadata.update(routing_layout.metadata())
    metadata["idRoutingStrategy"] = strategy
    if neo4j_resolved:
        metadata["neo4jNativeIdsResolved"] = True
        metadata["neo4jNativeIdFunction"] = "id"

    temporary = output.with_name(f".{output.name}.tmp")
    started = time.monotonic()
    last_report = started
    node_count = 0
    relationship_count = 0
    if progress_source:
        _progress(progress_source, f"{stage}: writing {output}")
    try:
        with GraphJsonLinesWriter(temporary, metadata=metadata) as writer:
            for node in open_graph_reader(source).iter_nodes():
                node_count += 1
                global_id = node_mapping.get(node.uuid, node.uuid)
                writer.write_node(
                    NodeRecord(
                        uuid=global_id,
                        type=node.type,
                        source=node.source,
                        properties=node.properties,
                    )
                )
                now = time.monotonic()
                if progress_source and now - last_report >= PROGRESS_INTERVAL_SECONDS:
                    _progress(
                        progress_source,
                        f"{stage}: wrote {node_count:,} nodes "
                        f"({now - started:,.1f}s elapsed)",
                    )
                    last_report = now
            for relationship in open_graph_reader(source).iter_relationships():
                relationship_count += 1
                writer.write_relationship(
                    RelationshipRecord(
                        uuid=relationship_mapping.get(relationship.uuid, relationship.uuid),
                        type=relationship.type,
                        source=relationship.source,
                        from_uuid=node_mapping.get(relationship.from_uuid, relationship.from_uuid),
                        to_uuid=node_mapping.get(relationship.to_uuid, relationship.to_uuid),
                        properties=relationship.properties,
                    )
                )
                now = time.monotonic()
                if progress_source and now - last_report >= PROGRESS_INTERVAL_SECONDS:
                    _progress(
                        progress_source,
                        f"{stage}: wrote {node_count:,} nodes and "
                        f"{relationship_count:,} relationships "
                        f"({now - started:,.1f}s elapsed)",
                    )
                    last_report = now
        temporary.replace(output)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    if progress_source:
        _progress(
            progress_source,
            f"{stage}: completed {node_count:,} nodes and "
            f"{relationship_count:,} relationships "
            f"({time.monotonic() - started:,.1f}s)",
        )
    return output


def _load_neo4j_fresh(
    *,
    input_graph: str | Path,
    resolved_output: str | Path,
    adapter: Neo4jHttpAdapter,
    routing_layout: BackendLayout,
    batch_size: int,
    id_map_output: str | Path,
) -> tuple[Path, dict[str, int | str]]:
    _progress(adapter.source_name, f"loading graph from {Path(input_graph).resolve()}")
    original_directory = build_endpoint_directory(
        input_graph,
        routing_layout,
        progress_source=adapter.source_name,
    )
    node_types, relationship_types, _, _ = inspect_source(
        input_graph,
        adapter.source_name,
        original_directory,
        routing_layout,
        progress_source=adapter.source_name,
    )
    _progress(adapter.source_name, "resetting database")
    adapter.reset(batch_size=batch_size)
    _progress(adapter.source_name, "creating proxy constraint")
    adapter.prepare(node_types, relationship_types)

    node_mapping: dict[int, int] = {}
    node_count = 0
    local_nodes = (
        node
        for node in open_graph_reader(input_graph).iter_nodes()
        if routing_layout.decode_source(node.uuid) == adapter.source_name
    )
    for batch in progress_batched(
        local_nodes,
        batch_size,
        source=adapter.source_name,
        stage="native nodes",
    ):
        node_mapping.update(adapter.insert_native_nodes(batch, routing_layout))
        node_count += len(batch)

    resolved = Path(resolved_output).expanduser().resolve()
    with tempfile.TemporaryDirectory(prefix="cpgdb-native-id-") as directory:
        node_resolved = Path(directory) / "nodes-resolved.jsonl"
        _rewrite_graph_ids(
            input_graph,
            node_resolved,
            routing_layout,
            node_mapping=node_mapping,
            neo4j_resolved=True,
            progress_source=adapter.source_name,
            stage="resolve native node IDs",
        )
        endpoint_directory = build_endpoint_directory(
            node_resolved,
            routing_layout,
            progress_source=adapter.source_name,
        )
        _, _, proxy_ids, local_endpoint_types = inspect_source(
            node_resolved,
            adapter.source_name,
            endpoint_directory,
            routing_layout,
            progress_source=adapter.source_name,
        )

        proxy_count = 0
        proxy_nodes = (
            _build_proxy_node(adapter.source_name, global_id, endpoint_directory[global_id])
            for global_id in sorted(proxy_ids)
        )
        for batch in progress_batched(
            proxy_nodes,
            batch_size,
            source=adapter.source_name,
            stage="proxy nodes",
        ):
            adapter.insert_proxy_nodes(batch)
            proxy_count += len(batch)

        relationship_mapping: dict[int, int] = {}
        relationship_count = 0
        local_relationships = (
            relationship
            for relationship in open_graph_reader(node_resolved).iter_relationships()
            if relationship_stored_on(
                relationship, adapter.source_name, endpoint_directory, routing_layout
            )
        )
        for batch in progress_batched(
            local_relationships,
            batch_size,
            source=adapter.source_name,
            stage="relationships",
        ):
            relationship_mapping.update(
                adapter.insert_relationships(
                    batch,
                    local_endpoint_types,
                    endpoint_directory,
                    routing_layout,
                )
            )
            relationship_count += len(batch)

        _rewrite_graph_ids(
            node_resolved,
            resolved,
            routing_layout,
            relationship_mapping=relationship_mapping,
            neo4j_resolved=True,
            progress_source=adapter.source_name,
            stage="resolve native relationship IDs",
        )

    id_map = write_native_id_map(
        id_map_output,
        node_mapping=node_mapping,
        relationship_mapping=relationship_mapping,
    )
    _progress(adapter.source_name, f"native ID map written to {id_map}")
    return resolved, {
        "nodes": node_count,
        "proxies": proxy_count,
        "relationships": relationship_count,
        "native_node_ids": len(node_mapping),
        "native_relationship_ids": len(relationship_mapping),
        "id_map": str(id_map),
    }


def load_source(
    *,
    input_graph: str | Path,
    source: str,
    adapter: ArangoHttpAdapter,
    endpoint_directory: dict[int, EndpointInfo],
    routing_layout: BackendLayout,
    reset: bool,
    batch_size: int,
) -> tuple[int, int, int]:
    node_types, relationship_types, proxy_ids, local_endpoint_types = inspect_source(
        input_graph,
        source,
        endpoint_directory,
        routing_layout,
        progress_source=source,
    )
    if not node_types and not relationship_types:
        _progress(source, "no records are placed on this backend")
        return 0, 0, 0

    if reset:
        _progress(source, "resetting database")
        adapter.reset(batch_size=batch_size)
    _progress(source, "creating collections")
    adapter.prepare(node_types, relationship_types)

    node_count = 0
    local_nodes = (
        node
        for node in open_graph_reader(input_graph).iter_nodes()
        if routing_layout.decode_source(node.uuid) == source
    )
    for batch in progress_batched(
        local_nodes,
        batch_size,
        source=source,
        stage="native nodes",
    ):
        adapter.insert_nodes(batch)
        node_count += len(batch)

    proxy_count = 0
    proxy_nodes = (
        _build_proxy_node(source, global_id, endpoint_directory[global_id])
        for global_id in sorted(proxy_ids)
    )
    for batch in progress_batched(
        proxy_nodes,
        batch_size,
        source=source,
        stage="proxy nodes",
    ):
        adapter.insert_nodes(batch)
        proxy_count += len(batch)

    relationship_count = 0
    local_relationships = (
        relationship
        for relationship in open_graph_reader(input_graph).iter_relationships()
        if relationship_stored_on(relationship, source, endpoint_directory, routing_layout)
    )
    for batch in progress_batched(
        local_relationships,
        batch_size,
        source=source,
        stage="relationships",
    ):
        adapter.insert_relationships(batch, local_endpoint_types)
        relationship_count += len(batch)

    _progress(source, "building UUID indexes after bulk import")
    adapter.finalize(node_types, relationship_types)
    _progress(source, "load completed")

    return node_count, proxy_count, relationship_count


def inspect_gpu_projection(
    input_graph: str | Path,
    endpoint_directory: dict[int, EndpointInfo],
    routing_layout: BackendLayout,
) -> dict[str, int]:
    """Count records that the C++ runtime materializes into the cuGraph projection."""
    gpu_nodes = sum(1 for endpoint in endpoint_directory.values() if endpoint.source == "GPU")
    gpu_edges = 0
    projection_vertices = {
        global_id for global_id, endpoint in endpoint_directory.items() if endpoint.source == "GPU"
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
    resolved_output: str | Path | None = None,
    id_map_output: str | Path | None = None,
) -> dict[str, dict[str, int | str]]:
    if batch_size <= 0:
        raise ValueError("batch_size must be positive")
    connections = read_backend_connections(backend_config)
    enabled_backends = read_enabled_backend_names(backend_config)
    routing_layout = read_id_routing_layout(backend_config)
    validate_graph_routing_layout(input_graph, routing_layout)
    selected = set(only_sources or enabled_backends)
    unknown = selected - enabled_backends
    if unknown:
        raise ValueError(f"Selected backends are not enabled/configured: {sorted(unknown)}")

    working_graph = Path(input_graph).expanduser().resolve()
    result: dict[str, dict[str, int | str]] = {}
    metadata = open_graph_reader(working_graph).metadata()
    _progress(
        "orchestrator",
        f"selected backends: {', '.join(sorted(selected))}; input graph: {working_graph}",
    )

    if "Neo4J" in selected:
        if metadata.get("neo4jNativeIdsResolved"):
            if reset:
                raise ValueError(
                    "A graph with resolved Neo4j native IDs cannot be reset/reloaded because Neo4j may "
                    "assign different native IDs. Regenerate from the unresolved source graph."
                )
            result["Neo4J"] = {
                "nodes": 0,
                "proxies": 0,
                "relationships": 0,
                "reused_existing_native_ids": 1,
            }
        else:
            if not reset:
                raise ValueError(
                    "Fresh Neo4j native-ID loading requires --reset so returned id(n)/id(r) values "
                    "unambiguously define the resolved graph."
                )
            default_output = working_graph.with_name(f"{working_graph.stem}.resolved.jsonl")
            resolved = Path(resolved_output).expanduser().resolve() if resolved_output else default_output
            default_id_map = resolved.with_name(f"{resolved.stem}.id-map.jsonl")
            native_id_map = (
                Path(id_map_output).expanduser().resolve() if id_map_output else default_id_map
            )
            working_graph, neo_result = _load_neo4j_fresh(
                input_graph=working_graph,
                resolved_output=resolved,
                adapter=Neo4jHttpAdapter(connections["Neo4J"]),
                routing_layout=routing_layout,
                batch_size=batch_size,
                id_map_output=native_id_map,
            )
            neo_result["resolved_graph"] = str(working_graph)
            result["Neo4J"] = neo_result
            _progress(
                "orchestrator",
                "Neo4J finished; ArangoDB can start only now because its proxy and edge IDs "
                "must use the resolved Neo4J native IDs",
            )

    endpoint_directory = build_endpoint_directory(
        working_graph,
        routing_layout,
        progress_source="orchestrator",
    )
    if "ArangoDB" in selected:
        adapter = ArangoHttpAdapter(connections["ArangoDB"])
        nodes, proxies, relationships = load_source(
            input_graph=working_graph,
            source="ArangoDB",
            adapter=adapter,
            endpoint_directory=endpoint_directory,
            routing_layout=routing_layout,
            reset=reset,
            batch_size=batch_size,
        )
        result["ArangoDB"] = {
            "nodes": nodes,
            "proxies": proxies,
            "relationships": relationships,
        }
    if "GPU" in selected:
        result["GPU"] = inspect_gpu_projection(working_graph, endpoint_directory, routing_layout)
    return result
