from __future__ import annotations

import json
import re
import sqlite3
import time
import urllib.parse
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from itertools import islice
from pathlib import Path
import threading
from typing import Any, Iterable, Iterator, Mapping, TypeVar

import requests

from .domain import sanitize_name
from .id_codec import (
    BackendLayout,
    cache_residency,
    canonical_global_id,
    gpu_location_bit,
    has_location,
    routing_names,
    to_signed_storage,
)
from .model import NodeRecord, RelationshipRecord
from .reader import open_graph_reader

T = TypeVar("T")
PROXY_NODE_TYPE = "Proxy"
NEO4J_LOCAL_ID_PROPERTY = "localId"
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
    bolt_endpoint: str = ""


def batched(values: Iterable[T], size: int) -> Iterator[list[T]]:
    if size <= 0:
        raise ValueError("batch size must be positive")
    iterator = iter(values)
    while batch := list(islice(iterator, size)):
        yield batch


def _progress(source: str, message: str) -> None:
    print(f"[polygraph][load][{source}] {message}", flush=True)


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
    raw_body: str | bytes | None = None,
    auth: tuple[str, str] | None = None,
    expected: tuple[int, ...] = (200,),
    timeout: int = 300,
    content_type: str = "application/json",
    attempts: int = 8,
    retry_delay: float = 2.0,
    session: requests.Session | None = None,
) -> Any:
    if payload is not None and raw_body is not None:
        raise ValueError("payload and raw_body are mutually exclusive")

    body: str | bytes | None
    if raw_body is not None:
        body = raw_body
    elif payload is not None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    else:
        body = None

    transient_http_codes = {429, 502, 503, 504}
    client = session or requests
    last_error: BaseException | None = None

    for attempt in range(1, attempts + 1):
        try:
            response = client.request(
                method,
                url,
                data=body,
                auth=auth,
                headers={"Accept": "application/json", "Content-Type": content_type},
                timeout=timeout,
            )
        except requests.RequestException as exc:
            if attempt == attempts:
                raise RuntimeError(
                    f"{method} {url} failed after {attempts} attempts: {exc}"
                ) from exc
            last_error = exc
        else:
            if response.status_code in expected:
                return response.json() if response.content else {}
            if response.status_code not in transient_http_codes or attempt == attempts:
                raise RuntimeError(
                    f"{method} {url}: HTTP {response.status_code}: {response.text}"
                )
            last_error = RuntimeError(
                f"{method} {url}: transient HTTP {response.status_code}: {response.text}"
            )

        time.sleep(retry_delay * min(attempt, 5))

    raise RuntimeError(f"{method} {url} failed: {last_error}")


def read_backend_connections(config_path: str | Path) -> dict[str, BackendConnection]:
    raw = json.loads(Path(config_path).read_text(encoding="utf-8"))
    result: dict[str, BackendConnection] = {}
    for entry in raw.get("backends", []):
        if not entry.get("enabled", True):
            continue
        source = str(entry.get("id", ""))
        if source not in {"Neo4J", "ArangoDB", "PostgreSQL"}:
            continue
        connection = entry.get("connection", {})
        credentials = connection.get("credentials", {})
        result[source] = BackendConnection(
            source=source,
            endpoint=str(connection.get("endpoint", "")),
            database=str(connection.get("database", "")),
            username=str(credentials.get("username", "")),
            password=str(credentials.get("password", "")),
            bolt_endpoint=str(entry.get("bolt_endpoint", "")),
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

    enabled_ids = {
        str(entry.get("id", ""))
        for entry in raw.get("backends", [])
        if entry.get("enabled", True)
    }
    # idRouting describes authoritative owners only. Cache-only sources (Redis,
    # mmap) and device-specific GPU ids must not be fed into BackendLayout.
    enabled_owners: set[str] = set()
    if any(value in enabled_ids for value in {"Neo4J", "Neo4j", "NEO4J"}):
        enabled_owners.add("Neo4J")
    if any(value in enabled_ids for value in {"ArangoDB", "Arango", "ARANGO"}):
        enabled_owners.add("ArangoDB")
    if any(value in enabled_ids for value in {"PostgreSQL", "Postgres", "POSTGRESQL"}):
        enabled_owners.add("PostgreSQL")
    if any(value == "GPU" or value.startswith("GPU:") for value in enabled_ids):
        enabled_owners.add("GPU")
    if any(value in enabled_ids for value in {"Memgraph", "MEMGRAPH"}):
        enabled_owners.add("Memgraph")

    disabled_routes = set(layout.backends) - enabled_owners
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


class EndpointDirectory:
    """Disk-backed node metadata used by large graph loaders.

    The old loader kept one Python object per node plus large proxy/mapping sets.
    At SF10 this already consumed tens of GiB; at SF100+ it is not viable.  This
    index keeps loader RAM bounded and stores canonical IDs so cache-residency
    reconfiguration does not invalidate the lookup key.
    """

    def __init__(self, path: str | Path):
        self.path = Path(path).expanduser().resolve()
        self.connection = sqlite3.connect(self.path)
        self.connection.execute("PRAGMA journal_mode=OFF")
        self.connection.execute("PRAGMA synchronous=OFF")
        self.connection.execute("PRAGMA temp_store=FILE")
        self.connection.execute("PRAGMA cache_size=-65536")  # ~64 MiB
        self.connection.execute("PRAGMA locking_mode=EXCLUSIVE")

    def close(self) -> None:
        self.connection.close()

    def __enter__(self) -> "EndpointDirectory":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    @staticmethod
    def _stored_id(value: int) -> int:
        return to_signed_storage(canonical_global_id(value))

    def get(self, value: int, default: EndpointInfo | None = None) -> EndpointInfo | None:
        row = self.connection.execute(
            "SELECT id,type,source FROM nodes WHERE id=?",
            (self._stored_id(value),),
        ).fetchone()
        if row is None:
            return default
        stored_id, type_name, source = row
        return EndpointInfo(
            global_id=int(stored_id) & ((1 << 64) - 1),
            type=str(type_name),
            source=str(source),
        )

    def __getitem__(self, value: int) -> EndpointInfo:
        result = self.get(value)
        if result is None:
            raise KeyError(value)
        return result

    def get_types(self, values: Iterable[int]) -> dict[int, str]:
        originals = list(dict.fromkeys(int(value) for value in values))
        canonical_to_originals: dict[int, list[int]] = defaultdict(list)
        for value in originals:
            canonical_to_originals[self._stored_id(value)].append(value)
        result: dict[int, str] = {}
        signed_ids = list(canonical_to_originals)
        for offset in range(0, len(signed_ids), 900):
            chunk = signed_ids[offset : offset + 900]
            placeholders = ",".join("?" for _ in chunk)
            for stored_id, type_name in self.connection.execute(
                f"SELECT id,type FROM nodes WHERE id IN ({placeholders})", chunk
            ):
                for original in canonical_to_originals[int(stored_id)]:
                    result[original] = str(type_name)
        return result

    def node_types_for_source(self, source: str) -> set[str]:
        return {
            str(row[0])
            for row in self.connection.execute(
                "SELECT DISTINCT type FROM nodes WHERE source=?", (source,)
            )
        }

    def create_proxy_table(self, source: str) -> str:
        table = f"proxies_{sanitize_name(source).lower()}"
        self.connection.execute(f"DROP TABLE IF EXISTS {table}")
        self.connection.execute(f"CREATE TABLE {table}(id INTEGER PRIMARY KEY)")
        self.connection.commit()
        return table

    def add_proxy_ids(self, table: str, values: Iterable[int]) -> None:
        rows = [(self._stored_id(value),) for value in values]
        if rows:
            self.connection.executemany(
                f"INSERT OR IGNORE INTO {table}(id) VALUES(?)", rows
            )

    def iter_proxy_ids(self, table: str) -> Iterator[int]:
        for (stored_id,) in self.connection.execute(f"SELECT id FROM {table} ORDER BY id"):
            yield int(stored_id) & ((1 << 64) - 1)

    def proxy_count(self, table: str) -> int:
        return int(self.connection.execute(f"SELECT count(*) FROM {table}").fetchone()[0])

    def projection_stats(self, graph_path: str | Path, *, device_id: int) -> dict[str, int]:
        bit = gpu_location_bit(device_id)
        table = f"gpu_projection_{device_id}"
        self.connection.execute(f"DROP TABLE IF EXISTS {table}")
        self.connection.execute(f"CREATE TABLE {table}(id INTEGER PRIMARY KEY, native INTEGER NOT NULL)")
        batch: list[tuple[int, int]] = []
        native_nodes = 0
        for node in open_graph_reader(graph_path).iter_nodes():
            if has_location(node.uuid, bit):
                batch.append((self._stored_id(node.uuid), 1))
                native_nodes += 1
                if len(batch) >= 50_000:
                    self.connection.executemany(
                        f"INSERT OR IGNORE INTO {table}(id,native) VALUES(?,?)", batch
                    )
                    batch.clear()
        if batch:
            self.connection.executemany(
                f"INSERT OR IGNORE INTO {table}(id,native) VALUES(?,?)", batch
            )
        self.connection.commit()

        gpu_edges = 0
        endpoints: list[tuple[int, int]] = []
        for relationship in open_graph_reader(graph_path).iter_relationships():
            if has_location(relationship.from_uuid, bit) or has_location(relationship.to_uuid, bit):
                gpu_edges += 1
                endpoints.append((self._stored_id(relationship.from_uuid), 0))
                endpoints.append((self._stored_id(relationship.to_uuid), 0))
                if len(endpoints) >= 100_000:
                    self.connection.executemany(
                        f"INSERT OR IGNORE INTO {table}(id,native) VALUES(?,?)", endpoints
                    )
                    endpoints.clear()
        if endpoints:
            self.connection.executemany(
                f"INSERT OR IGNORE INTO {table}(id,native) VALUES(?,?)", endpoints
            )
        self.connection.commit()
        projection_vertices = int(
            self.connection.execute(f"SELECT count(*) FROM {table}").fetchone()[0]
        )
        self.connection.execute(f"DROP TABLE {table}")
        self.connection.commit()
        return {
            "device_id": device_id,
            "nodes": native_nodes,
            "proxies": projection_vertices - native_nodes,
            "relationships": gpu_edges,
            "projection_vertices": projection_vertices,
        }


def build_endpoint_directory(
    input_graph: str | Path,
    routing_layout: BackendLayout,
    *,
    progress_source: str | None = None,
    index_path: str | Path | None = None,
) -> EndpointDirectory:
    source = Path(input_graph).expanduser().resolve()
    output = (
        Path(index_path).expanduser().resolve()
        if index_path is not None
        else source.with_name(f".{source.name}.load-index.sqlite3")
    )
    output.unlink(missing_ok=True)
    directory = EndpointDirectory(output)
    directory.connection.executescript(
        """
        CREATE TABLE nodes(
            id INTEGER PRIMARY KEY,
            type TEXT NOT NULL,
            source TEXT NOT NULL
        );
        """
    )
    started = time.monotonic()
    last_report = started
    count = 0
    batch: list[tuple[int, str, str]] = []
    try:
        for node in open_graph_reader(source).iter_nodes():
            batch.append((
                EndpointDirectory._stored_id(node.uuid),
                node.type,
                routing_layout.decode_source(node.uuid),
            ))
            count += 1
            if len(batch) >= 50_000:
                try:
                    directory.connection.executemany(
                        "INSERT INTO nodes(id,type,source) VALUES(?,?,?)", batch
                    )
                except sqlite3.IntegrityError as exc:
                    raise ValueError("Duplicate canonical node ID while building loader index") from exc
                batch.clear()
            now = time.monotonic()
            if progress_source and now - last_report >= PROGRESS_INTERVAL_SECONDS:
                _progress(
                    progress_source,
                    f"endpoint index: indexed {count:,} nodes on disk "
                    f"({now - started:,.1f}s elapsed)",
                )
                last_report = now
        if batch:
            directory.connection.executemany(
                "INSERT INTO nodes(id,type,source) VALUES(?,?,?)", batch
            )
        directory.connection.commit()
    except Exception:
        directory.close()
        output.unlink(missing_ok=True)
        raise
    if progress_source:
        _progress(
            progress_source,
            f"endpoint index: completed {count:,} nodes on disk "
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
        self._http = requests.Session()
        self._driver = None
        if connection.bolt_endpoint:
            from neo4j import GraphDatabase

            self._driver = GraphDatabase.driver(
                connection.bolt_endpoint,
                auth=self.auth,
                max_connection_pool_size=8,
            )

    def _cypher(self, statement: str, parameters: dict[str, Any] | None = None) -> dict[str, Any]:
        if self._driver is not None:
            try:
                with self._driver.session(database=self.connection.database or "neo4j") as session:
                    result = session.run(statement, parameters or {})
                    result.consume()
                return {"results": [{"data": []}], "errors": []}
            except Exception as exc:
                code = getattr(exc, "code", exc.__class__.__name__)
                message = str(exc)
                raise Neo4jQueryError([{"code": code, "message": message}]) from exc

        response = _request_json(
            "POST",
            self.connection.endpoint,
            payload={"statements": [{"statement": statement, "parameters": parameters or {}}]},
            auth=self.auth,
            expected=(200, 201),
            timeout=900,
            session=self._http,
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

        if self._driver is not None:
            # One implicit Bolt query lets Neo4j perform the delete in server-side
            # subtransactions instead of paying one client round-trip per batch.
            delete_batch_size = min(max(batch_size, 1_000), 10_000)
            self._cypher(
                "MATCH (n) "
                "CALL (n) { DETACH DELETE n } "
                f"IN TRANSACTIONS OF {delete_batch_size} ROWS"
            )
            return

        # HTTP fallback for configurations that do not provide a Bolt endpoint.
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

    def prepare(self, node_types: Iterable[str], _relationship_types: Iterable[str]) -> None:
        del node_types
        # Real Neo4j vertices use a stable, application-controlled integer ID.
        # Proxy vertices retain the complete remote global ID in ``uuid``.
        self._cypher(
            "CREATE CONSTRAINT polygraph_vertex_local_id_unique IF NOT EXISTS "
            "FOR (n:POLYGRAPHVertex) REQUIRE n.localId IS UNIQUE"
        )
        self._cypher(
            "CREATE CONSTRAINT proxy_uuid_unique IF NOT EXISTS "
            "FOR (n:Proxy) REQUIRE n.uuid IS UNIQUE"
        )

    def insert_native_nodes(
        self,
        nodes: list[NodeRecord],
        routing_layout: BackendLayout,
        *,
        first_local_id: int = 0,
    ) -> dict[int, int]:
        if first_local_id < 0:
            raise ValueError("first_local_id must be non-negative")

        grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in nodes:
            # The low payload is the stable backend-local ID. Cache-residency
            # bits must never force a Neo4j ID rewrite because cache artifacts
            # and the other stores refer to the same canonical payload.
            local_id = routing_layout.local_id(node.uuid)
            props = dict(node.properties)
            props.pop("uuid", None)
            props.pop(NEO4J_LOCAL_ID_PROPERTY, None)
            grouped[node.type].append(
                {
                    "local_id": local_id,
                    "type": node.type,
                    "source": self.source_name,
                    "props": props,
                }
            )

        remapped: dict[int, int] = {node.uuid: node.uuid for node in nodes}
        for type_name, rows in sorted(grouped.items()):
            label = sanitize_name(type_name)
            self._cypher(
                " ".join(
                    [
                        "UNWIND $rows AS row",
                        f"CREATE (n:{label}:POLYGRAPHVertex:__GraphNode)",
                        "SET n = row.props, n.localId = row.local_id, "
                        "n.type = row.type, n.source = row.source, n.is_proxy = false",
                    ]
                ),
                {"rows": rows},
            )
        return remapped

    def insert_proxy_nodes(self, nodes: list[NodeRecord]) -> None:
        rows = []
        for node in nodes:
            props = dict(node.properties)
            props.pop("uuid", None)
            props.pop(NEO4J_LOCAL_ID_PROPERTY, None)
            rows.append(
                {
                    "uuid": to_signed_storage(canonical_global_id(node.uuid)),
                    "type": node.type,
                    "source": self.source_name,
                    "props": props,
                }
            )
        if not rows:
            return
        self._cypher(
            "UNWIND $rows AS row "
            "MERGE (n:Proxy:POLYGRAPHVertex:__GraphNode {uuid: row.uuid}) "
            "SET n += row.props, n.type = row.type, n.source = row.source, n.is_proxy = true",
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
        *,
        first_local_id: int = 0,
    ) -> dict[int, int]:
        if first_local_id < 0:
            raise ValueError("first_local_id must be non-negative")

        del local_endpoint_types, endpoint_directory  # routing is encoded in the GlobalId.
        grouped: dict[tuple[str, bool, bool, bool], list[dict[str, Any]]] = defaultdict(list)
        remapped: dict[int, int] = {}
        for relationship in relationships:
            from_source = routing_layout.decode_source(relationship.from_uuid)
            to_source = routing_layout.decode_source(relationship.to_uuid)
            primary_is_native = routing_layout.decode_source(relationship.uuid) == self.source_name
            from_proxy = from_source != self.source_name
            to_proxy = to_source != self.source_name
            local_id = routing_layout.local_id(relationship.uuid) if primary_is_native else None
            props = {
                "type": relationship.type,
                "source": relationship.source,
                **{
                    k: v
                    for k, v in relationship.properties.items()
                    if k not in {"uuid", NEO4J_LOCAL_ID_PROPERTY}
                },
            }
            if primary_is_native:
                remapped[relationship.uuid] = relationship.uuid
            grouped[
                (
                    relationship.type,
                    primary_is_native,
                    from_proxy,
                    to_proxy,
                )
            ].append(
                {
                    "stored_id": to_signed_storage(canonical_global_id(relationship.uuid)),
                    "local_id": local_id,
                    "from_id": (
                        routing_layout.local_id(relationship.from_uuid)
                        if from_source == self.source_name
                        else to_signed_storage(canonical_global_id(relationship.from_uuid))
                    ),
                    "to_id": (
                        routing_layout.local_id(relationship.to_uuid)
                        if to_source == self.source_name
                        else to_signed_storage(canonical_global_id(relationship.to_uuid))
                    ),
                    "props": props,
                }
            )

        for (
            rel_type,
            primary_is_native,
            from_proxy,
            to_proxy,
        ), rows in sorted(grouped.items()):
            id_assignment = (
                "SET r.localId = row.local_id REMOVE r.uuid"
                if primary_is_native
                else "SET r.uuid = row.stored_id REMOVE r.localId"
            )
            from_match = (
                "MATCH (a:Proxy {uuid: row.from_id})"
                if from_proxy
                else "MATCH (a:POLYGRAPHVertex {localId: row.from_id})"
            )
            to_match = (
                "MATCH (b:Proxy {uuid: row.to_id})"
                if to_proxy
                else "MATCH (b:POLYGRAPHVertex {localId: row.to_id})"
            )
            clauses = [
                "UNWIND $rows AS row",
                from_match,
                to_match,
                f"CREATE (a)-[r:{sanitize_name(rel_type)}]->(b)",
                "SET r = row.props",
                id_assignment,
            ]
            self._cypher("\n".join(clauses), {"rows": rows})
        return remapped

    def update_cache_residency(
        self, nodes: list[NodeRecord], routing_layout: BackendLayout
    ) -> None:
        rows = [
            {
                "local_id": routing_layout.local_id(node.uuid),
                "cache_residency": cache_residency(node.uuid),
            }
            for node in nodes
        ]
        if not rows:
            return
        self._cypher(
            "UNWIND $rows AS row "
            "MATCH (n:POLYGRAPHVertex {localId: row.local_id}) "
            "SET n.cache_residency = row.cache_residency",
            {"rows": rows},
        )


class ArangoHttpAdapter:
    def __init__(self, connection: BackendConnection, *, import_workers: int = 4):
        if import_workers <= 0:
            raise ValueError("import_workers must be positive")
        self.source_name = "ArangoDB"
        self.connection = connection
        self.import_workers = import_workers
        self._session_local = threading.local()
        self.auth = (connection.username, connection.password)
        marker = "/_db/"
        if marker in connection.endpoint:
            self.base_url = connection.endpoint.split(marker, 1)[0].rstrip("/")
        else:
            parsed = urllib.parse.urlsplit(connection.endpoint)
            self.base_url = f"{parsed.scheme}://{parsed.netloc}"
        self.database = connection.database or "university"

    def _session(self) -> requests.Session:
        session = getattr(self._session_local, "session", None)
        if session is None:
            session = requests.Session()
            self._session_local.session = session
        return session

    def system_api(self, path: str) -> str:
        return f"{self.base_url}{path}"

    def db_api(self, path: str) -> str:
        return f"{self.base_url}/_db/{urllib.parse.quote(self.database, safe='')}{path}"

    def ensure_database(self) -> None:
        payload = _request_json(
            "GET",
            self.system_api("/_api/database"),
            auth=self.auth,
            expected=(200,),
            session=self._session(),
        )
        if self.database not in payload.get("result", []):
            _request_json(
                "POST",
                self.system_api("/_api/database"),
                payload={"name": self.database},
                auth=self.auth,
                expected=(201, 202),
                session=self._session(),
            )

    def reset(self, *, batch_size: int = 1_000) -> None:
        del batch_size  # ArangoDB resets by recreating the whole database.
        _request_json(
            "DELETE",
            self.system_api(f"/_api/database/{urllib.parse.quote(self.database, safe='')}"),
            auth=self.auth,
            expected=(200, 202, 404),
            session=self._session(),
        )
        _request_json(
            "POST",
            self.system_api("/_api/database"),
            payload={"name": self.database},
            auth=self.auth,
            expected=(201, 202),
            session=self._session(),
        )

    def ensure_collection(self, name: str, *, edge: bool) -> None:
        _request_json(
            "POST",
            self.db_api("/_api/collection"),
            payload={"name": sanitize_name(name), "type": 3 if edge else 2},
            auth=self.auth,
            expected=(200, 201, 202, 409),
            session=self._session(),
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
            session=self._session(),
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
        if not documents:
            return
        query = urllib.parse.urlencode(
            {
                "collection": sanitize_name(collection),
                "type": "documents",
                "onDuplicate": "replace",
                "complete": "true",
            }
        )
        # ArangoDB's line-wise document import can stream/parse records without
        # materializing a complete JSON array on the server. Compact JSON also
        # cuts request bytes and serialization overhead for large graph batches.
        body = "\n".join(
            json.dumps(document, ensure_ascii=False, separators=(",", ":"))
            for document in documents
        ) + "\n"
        payload = _request_json(
            "POST",
            f"{self.db_api('/_api/import')}?{query}",
            raw_body=body,
            auth=self.auth,
            expected=(201,),
            timeout=900,
            session=self._session(),
        )
        if payload.get("errors", 0):
            raise RuntimeError(f"Arango import into {collection} reported errors: {payload}")

    def _import_grouped(self, grouped: dict[str, list[dict[str, Any]]]) -> None:
        items = [(collection, documents) for collection, documents in sorted(grouped.items()) if documents]
        if not items:
            return
        if self.import_workers == 1:
            for collection, documents in items:
                self._import_batch(collection, documents)
            return

        # Do not rely on the input graph interleaving different types. LDBC-style
        # inputs are often grouped by type, so split a large single collection too.
        # Aim for roughly one import task per worker across this logical batch.
        total_documents = sum(len(documents) for _, documents in items)
        target_chunk_size = max(1_000, (total_documents + self.import_workers - 1) // self.import_workers)
        tasks: list[tuple[str, list[dict[str, Any]]]] = []
        for collection, documents in items:
            tasks.extend(
                (collection, chunk)
                for chunk in batched(documents, target_chunk_size)
            )

        if len(tasks) == 1:
            collection, documents = tasks[0]
            self._import_batch(collection, documents)
            return

        workers = min(self.import_workers, len(tasks))
        with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="arango-import") as executor:
            futures = [
                executor.submit(self._import_batch, collection, documents)
                for collection, documents in tasks
            ]
            for future in futures:
                future.result()

    def insert_nodes(self, nodes: list[NodeRecord]) -> None:
        grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in nodes:
            grouped[node.type].append(
                {
                    **node.properties,
                    "_key": str(canonical_global_id(node.uuid)),
                    "uuid": to_signed_storage(canonical_global_id(node.uuid)),
                    "type": node.type,
                    "source": node.source,
                }
            )
        self._import_grouped(grouped)

    def update_cache_residency(self, nodes: list[NodeRecord]) -> None:
        grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for node in nodes:
            grouped[node.type].append({
                "_key": str(canonical_global_id(node.uuid)),
                "cache_residency": cache_residency(node.uuid),
            })
        for collection, documents in grouped.items():
            if not documents:
                continue
            query = urllib.parse.urlencode({
                "collection": sanitize_name(collection),
                "type": "documents",
                "onDuplicate": "update",
                "complete": "true",
            })
            body = "\n".join(
                json.dumps(document, ensure_ascii=False, separators=(",", ":"))
                for document in documents
            ) + "\n"
            payload = _request_json(
                "POST",
                f"{self.db_api('/_api/import')}?{query}",
                raw_body=body,
                auth=self.auth,
                expected=(201,),
                timeout=900,
                session=self._session(),
            )
            if payload.get("errors", 0):
                raise RuntimeError(
                    f"Arango cache-residency update into {collection} reported errors: {payload}"
                )

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
                    "_key": str(canonical_global_id(relationship.uuid)),
                    "_from": f"{sanitize_name(local_endpoint_types[relationship.from_uuid])}/{canonical_global_id(relationship.from_uuid)}",
                    "_to": f"{sanitize_name(local_endpoint_types[relationship.to_uuid])}/{canonical_global_id(relationship.to_uuid)}",
                    "uuid": to_signed_storage(canonical_global_id(relationship.uuid)),
                    "type": relationship.type,
                    "source": relationship.source,
                }
            )
        self._import_grouped(grouped)


class PostgreSqlAdapter:
    """Bulk PostgreSQL loader using psycopg 3 COPY."""

    def __init__(self, connection: BackendConnection):
        try:
            import psycopg
        except ImportError as exc:
            raise RuntimeError(
                "PostgreSQL loading requires psycopg 3. Install tools/requirements.txt"
            ) from exc

        self.source_name = "PostgreSQL"
        self.connection = connection
        endpoint = connection.endpoint or "127.0.0.1:5432"
        if endpoint.startswith(("postgresql://", "postgres://")):
            parsed = urllib.parse.urlsplit(endpoint)
            host = parsed.hostname or "127.0.0.1"
            port = parsed.port or 5432
        else:
            host_port = endpoint.split("/", 1)[0]
            host, separator, port_text = host_port.rpartition(":")
            if not separator:
                host, port = host_port or "127.0.0.1", 5432
            else:
                host, port = host or "127.0.0.1", int(port_text)
        self._connection = psycopg.connect(
            host=host,
            port=port,
            dbname=connection.database or "polygraph",
            user=connection.username or "polygraph",
            password=connection.password or "polygraph",
            application_name="polygraph-loader",
        )
        self._fresh_load = False

    def reset(self, *, batch_size: int = 1_000) -> None:
        del batch_size
        # Keep the loader usable with an external PostgreSQL database that was
        # not created by the repository Docker initialization scripts.
        self.prepare((), ())
        with self._connection.cursor() as cursor:
            cursor.execute("TRUNCATE TABLE polygraph_edges, polygraph_nodes")
        self._connection.commit()
        self._fresh_load = True

    def prepare(self, _node_types: Iterable[str], _relationship_types: Iterable[str]) -> None:
        with self._connection.cursor() as cursor:
            cursor.execute(
                """
                CREATE TABLE IF NOT EXISTS polygraph_nodes(
                    id BIGINT PRIMARY KEY,
                    type TEXT NOT NULL,
                    source TEXT NOT NULL,
                    is_proxy BOOLEAN NOT NULL DEFAULT FALSE,
                    cache_residency INTEGER NOT NULL DEFAULT 0,
                    properties JSONB NOT NULL DEFAULT '{}'::jsonb
                )
                """
            )
            cursor.execute(
                """
                CREATE TABLE IF NOT EXISTS polygraph_edges(
                    id BIGINT PRIMARY KEY,
                    edge_type TEXT NOT NULL,
                    source TEXT NOT NULL,
                    from_id BIGINT NOT NULL,
                    from_type TEXT NOT NULL,
                    to_id BIGINT NOT NULL,
                    to_type TEXT NOT NULL,
                    properties JSONB NOT NULL DEFAULT '{}'::jsonb
                )
                """
            )
            cursor.execute(
                "CREATE TEMP TABLE IF NOT EXISTS polygraph_nodes_stage "
                "(LIKE polygraph_nodes INCLUDING DEFAULTS) ON COMMIT DELETE ROWS"
            )
            cursor.execute(
                "CREATE TEMP TABLE IF NOT EXISTS polygraph_edges_stage "
                "(LIKE polygraph_edges INCLUDING DEFAULTS) ON COMMIT DELETE ROWS"
            )
            # Maintaining these during a multi-billion-row COPY is considerably
            # slower than rebuilding them after the import.
            cursor.execute("DROP INDEX IF EXISTS polygraph_edges_from_idx")
            cursor.execute("DROP INDEX IF EXISTS polygraph_edges_to_idx")
            cursor.execute("DROP INDEX IF EXISTS polygraph_nodes_type_idx")
        self._connection.commit()

    def insert_nodes(self, nodes: list[NodeRecord]) -> None:
        if not nodes:
            return
        from psycopg.types.json import Jsonb

        target = "polygraph_nodes" if self._fresh_load else "polygraph_nodes_stage"
        with self._connection.cursor() as cursor:
            with cursor.copy(
                f"COPY {target}(id,type,source,is_proxy,cache_residency,properties) FROM STDIN"
            ) as copy:
                for node in nodes:
                    properties = dict(node.properties)
                    is_proxy = bool(properties.pop("is_proxy", False))
                    copy.write_row((
                        to_signed_storage(canonical_global_id(node.uuid)),
                        node.type,
                        self.source_name,
                        is_proxy,
                        cache_residency(node.uuid),
                        Jsonb(properties),
                    ))
            if not self._fresh_load:
                cursor.execute(
                    "INSERT INTO polygraph_nodes(id,type,source,is_proxy,cache_residency,properties) "
                    "SELECT id,type,source,is_proxy,cache_residency,properties FROM polygraph_nodes_stage "
                    "ON CONFLICT(id) DO UPDATE SET type=EXCLUDED.type, source=EXCLUDED.source, "
                    "is_proxy=EXCLUDED.is_proxy, cache_residency=EXCLUDED.cache_residency, "
                    "properties=EXCLUDED.properties"
                )
        self._connection.commit()

    def insert_relationships(
        self, relationships: list[RelationshipRecord], endpoint_types: dict[int, str]
    ) -> None:
        if not relationships:
            return
        from psycopg.types.json import Jsonb

        target = "polygraph_edges" if self._fresh_load else "polygraph_edges_stage"
        with self._connection.cursor() as cursor:
            with cursor.copy(
                f"COPY {target}(id,edge_type,source,from_id,from_type,to_id,to_type,properties) FROM STDIN"
            ) as copy:
                for relationship in relationships:
                    copy.write_row((
                        to_signed_storage(canonical_global_id(relationship.uuid)),
                        relationship.type,
                        relationship.source,
                        to_signed_storage(canonical_global_id(relationship.from_uuid)),
                        endpoint_types[relationship.from_uuid],
                        to_signed_storage(canonical_global_id(relationship.to_uuid)),
                        endpoint_types[relationship.to_uuid],
                        Jsonb(relationship.properties),
                    ))
            if not self._fresh_load:
                cursor.execute(
                    "INSERT INTO polygraph_edges(id,edge_type,source,from_id,from_type,to_id,to_type,properties) "
                    "SELECT id,edge_type,source,from_id,from_type,to_id,to_type,properties FROM polygraph_edges_stage "
                    "ON CONFLICT(id) DO UPDATE SET edge_type=EXCLUDED.edge_type, source=EXCLUDED.source, "
                    "from_id=EXCLUDED.from_id, from_type=EXCLUDED.from_type, to_id=EXCLUDED.to_id, "
                    "to_type=EXCLUDED.to_type, properties=EXCLUDED.properties"
                )
        self._connection.commit()

    def finalize(self, _node_types: Iterable[str], _relationship_types: Iterable[str]) -> None:
        with self._connection.cursor() as cursor:
            cursor.execute(
                "CREATE INDEX IF NOT EXISTS polygraph_edges_from_idx "
                "ON polygraph_edges(from_id,edge_type) INCLUDE(to_id,to_type)"
            )
            cursor.execute(
                "CREATE INDEX IF NOT EXISTS polygraph_edges_to_idx "
                "ON polygraph_edges(to_id,edge_type) INCLUDE(from_id,from_type)"
            )
            cursor.execute(
                "CREATE INDEX IF NOT EXISTS polygraph_nodes_type_idx ON polygraph_nodes(type,id)"
            )
            cursor.execute("ANALYZE polygraph_nodes")
            cursor.execute("ANALYZE polygraph_edges")
        self._connection.commit()

    def update_cache_residency(self, nodes: list[NodeRecord]) -> None:
        if not nodes:
            return
        with self._connection.cursor() as cursor:
            cursor.execute(
                "CREATE TEMP TABLE polygraph_residency_update("
                "id BIGINT PRIMARY KEY, cache_residency INTEGER NOT NULL) ON COMMIT DROP"
            )
            with cursor.copy(
                "COPY polygraph_residency_update(id,cache_residency) FROM STDIN"
            ) as copy:
                for node in nodes:
                    copy.write_row((
                        to_signed_storage(canonical_global_id(node.uuid)),
                        cache_residency(node.uuid),
                    ))
            cursor.execute(
                "UPDATE polygraph_nodes n SET cache_residency=u.cache_residency "
                "FROM polygraph_residency_update u WHERE n.id=u.id"
            )
        self._connection.commit()


@dataclass(frozen=True)
class SourceInspection:
    node_types: set[str]
    relationship_types: set[str]
    proxy_table: str
    proxy_count: int


def relationship_stored_on(
    relationship: RelationshipRecord,
    source: str,
    endpoint_directory: EndpointDirectory | dict[int, EndpointInfo] | None,
    routing_layout: BackendLayout,
) -> bool:
    """Store the primary edge plus an adjacency replica at both endpoint homes.

    Endpoint ownership is encoded directly in the GlobalId, so the hot scan does
    not need a Python/SQLite lookup for every edge. ``endpoint_directory`` is kept
    in the signature for compatibility with older callers/tests.
    """
    del endpoint_directory
    primary = routing_layout.decode_source(relationship.uuid)
    from_source = routing_layout.decode_source(relationship.from_uuid)
    to_source = routing_layout.decode_source(relationship.to_uuid)
    return source in {primary, from_source, to_source}


def inspect_source(
    input_graph: str | Path,
    source: str,
    endpoint_directory: EndpointDirectory,
    routing_layout: BackendLayout,
    *,
    progress_source: str | None = None,
) -> SourceInspection:
    node_types = endpoint_directory.node_types_for_source(source)
    relationship_types: set[str] = set()
    proxy_table = endpoint_directory.create_proxy_table(source)
    pending_proxy_ids: list[int] = []

    inspected = 0
    started = time.monotonic()
    last_report = started
    for relationship in open_graph_reader(input_graph).iter_relationships():
        inspected += 1
        primary = routing_layout.decode_source(relationship.uuid)
        from_source = routing_layout.decode_source(relationship.from_uuid)
        to_source = routing_layout.decode_source(relationship.to_uuid)
        if source in {primary, from_source, to_source}:
            relationship_types.add(relationship.type)
            if from_source != source:
                pending_proxy_ids.append(relationship.from_uuid)
            if to_source != source:
                pending_proxy_ids.append(relationship.to_uuid)
            if len(pending_proxy_ids) >= 50_000:
                endpoint_directory.add_proxy_ids(proxy_table, pending_proxy_ids)
                pending_proxy_ids.clear()

        now = time.monotonic()
        if progress_source and now - last_report >= PROGRESS_INTERVAL_SECONDS:
            if pending_proxy_ids:
                endpoint_directory.add_proxy_ids(proxy_table, pending_proxy_ids)
                pending_proxy_ids.clear()
            endpoint_directory.connection.commit()
            _progress(
                progress_source,
                f"placement scan: inspected {inspected:,} relationships; "
                f"selected {len(relationship_types):,} relationship types and "
                f"{endpoint_directory.proxy_count(proxy_table):,} proxies "
                f"({now - started:,.1f}s elapsed)",
            )
            last_report = now

    if pending_proxy_ids:
        endpoint_directory.add_proxy_ids(proxy_table, pending_proxy_ids)
    endpoint_directory.connection.commit()
    proxy_count = endpoint_directory.proxy_count(proxy_table)
    if proxy_count:
        node_types.add(PROXY_NODE_TYPE)
    if progress_source:
        _progress(
            progress_source,
            f"placement scan: completed {inspected:,} relationships, "
            f"{proxy_count:,} proxies ({time.monotonic() - started:,.1f}s)",
        )
    return SourceInspection(node_types, relationship_types, proxy_table, proxy_count)

def _load_neo4j_fresh(
    *,
    input_graph: str | Path,
    adapter: Neo4jHttpAdapter,
    routing_layout: BackendLayout,
    batch_size: int,
    endpoint_directory: EndpointDirectory,
) -> tuple[Path, dict[str, int | str]]:
    """Load Neo4j with stable 48-bit payload IDs using bounded host memory.

    New-format PolyGraph IDs already contain the stable backend-local payload.
    Therefore there is no old->new mapping to keep, sort, write, or use to
    rewrite a second full graph file.  That removes the SF10 OOM and makes host
    memory independent of |V| and |E| apart from bounded batches/SQLite cache.
    """
    graph = Path(input_graph).expanduser().resolve()
    _progress(adapter.source_name, f"loading graph from {graph}")
    inspection = inspect_source(
        graph,
        adapter.source_name,
        endpoint_directory,
        routing_layout,
        progress_source=adapter.source_name,
    )
    _progress(adapter.source_name, "resetting database")
    adapter.reset(batch_size=batch_size)
    _progress(adapter.source_name, "creating integer-ID constraints")
    adapter.prepare(inspection.node_types, inspection.relationship_types)

    node_count = 0
    local_nodes = (
        node
        for node in open_graph_reader(graph).iter_nodes()
        if routing_layout.decode_source(node.uuid) == adapter.source_name
    )
    for batch in progress_batched(
        local_nodes,
        batch_size,
        source=adapter.source_name,
        stage="native nodes",
    ):
        adapter.insert_native_nodes(batch, routing_layout, first_local_id=node_count)
        node_count += len(batch)

    proxy_count = 0
    proxy_ids = endpoint_directory.iter_proxy_ids(inspection.proxy_table)
    for id_batch in progress_batched(
        proxy_ids,
        batch_size,
        source=adapter.source_name,
        stage="proxy IDs",
    ):
        types = endpoint_directory.get_types(id_batch)
        nodes = [
            _build_proxy_node(
                adapter.source_name,
                global_id,
                EndpointInfo(
                    global_id=global_id,
                    type=types[global_id],
                    source=routing_layout.decode_source(global_id),
                ),
            )
            for global_id in id_batch
        ]
        adapter.insert_proxy_nodes(nodes)
        proxy_count += len(nodes)

    relationship_count = 0
    next_relationship_local_id = 0
    local_relationships = (
        relationship
        for relationship in open_graph_reader(graph).iter_relationships()
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
        adapter.insert_relationships(
            batch,
            {},
            endpoint_directory,
            routing_layout,
            first_local_id=next_relationship_local_id,
        )
        # localId is derived from the relationship payload, not this counter; the
        # counter is kept only for compatibility with the adapter API.
        next_relationship_local_id += len(batch)
        relationship_count += len(batch)

    _progress(
        adapter.source_name,
        "stable payload IDs loaded; no in-memory ID maps or resolved graph rewrite required",
    )
    return graph, {
        "nodes": node_count,
        "proxies": proxy_count,
        "relationships": relationship_count,
        "stable_payload_ids": 1,
        "integer_node_ids": node_count,
        "integer_relationship_ids": relationship_count,
    }

def load_source(
    *,
    input_graph: str | Path,
    source: str,
    adapter: Any,
    endpoint_directory: EndpointDirectory,
    routing_layout: BackendLayout,
    reset: bool,
    batch_size: int,
) -> tuple[int, int, int]:
    inspection = inspect_source(
        input_graph,
        source,
        endpoint_directory,
        routing_layout,
        progress_source=source,
    )
    if not inspection.node_types and not inspection.relationship_types:
        _progress(source, "no records are placed on this backend")
        return 0, 0, 0

    if reset:
        _progress(source, "resetting database")
        adapter.reset(batch_size=batch_size)
    _progress(source, "creating collections")
    adapter.prepare(inspection.node_types, inspection.relationship_types)

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
    for id_batch in progress_batched(
        endpoint_directory.iter_proxy_ids(inspection.proxy_table),
        batch_size,
        source=source,
        stage="proxy IDs",
    ):
        types = endpoint_directory.get_types(id_batch)
        nodes = [
            _build_proxy_node(
                source,
                global_id,
                EndpointInfo(
                    global_id=global_id,
                    type=types[global_id],
                    source=routing_layout.decode_source(global_id),
                ),
            )
            for global_id in id_batch
        ]
        adapter.insert_nodes(nodes)
        proxy_count += len(nodes)

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
        endpoint_ids = [
            endpoint
            for relationship in batch
            for endpoint in (relationship.from_uuid, relationship.to_uuid)
            if routing_layout.decode_source(endpoint) == source
        ]
        local_types = endpoint_directory.get_types(endpoint_ids)
        endpoint_types: dict[int, str] = {}
        for relationship in batch:
            for endpoint in (relationship.from_uuid, relationship.to_uuid):
                if routing_layout.decode_source(endpoint) == source:
                    endpoint_types[endpoint] = local_types[endpoint]
                else:
                    endpoint_types[endpoint] = PROXY_NODE_TYPE
        adapter.insert_relationships(batch, endpoint_types)
        relationship_count += len(batch)

    _progress(source, "building UUID indexes after bulk import")
    adapter.finalize(inspection.node_types, inspection.relationship_types)
    _progress(source, "load completed")

    return node_count, proxy_count, relationship_count



def load_postgresql_source(
    *,
    input_graph: str | Path,
    adapter: PostgreSqlAdapter,
    endpoint_directory: EndpointDirectory,
    routing_layout: BackendLayout,
    reset: bool,
    batch_size: int,
) -> tuple[int, int, int]:
    source = "PostgreSQL"
    inspection = inspect_source(
        input_graph,
        source,
        endpoint_directory,
        routing_layout,
        progress_source=source,
    )
    if not inspection.node_types and not inspection.relationship_types:
        _progress(source, "no records are placed on this backend")
        return 0, 0, 0

    if reset:
        _progress(source, "resetting database")
        adapter.reset(batch_size=batch_size)
    _progress(source, "creating relational graph tables")
    adapter.prepare(inspection.node_types, inspection.relationship_types)

    node_count = 0
    local_nodes = (
        node
        for node in open_graph_reader(input_graph).iter_nodes()
        if routing_layout.decode_source(node.uuid) == source
    )
    for batch in progress_batched(local_nodes, batch_size, source=source, stage="native nodes"):
        adapter.insert_nodes(batch)
        node_count += len(batch)

    proxy_count = 0
    for id_batch in progress_batched(
        endpoint_directory.iter_proxy_ids(inspection.proxy_table),
        batch_size,
        source=source,
        stage="proxy IDs",
    ):
        types = endpoint_directory.get_types(id_batch)
        nodes: list[NodeRecord] = []
        for global_id in id_batch:
            remote = endpoint_directory[global_id]
            nodes.append(
                NodeRecord(
                    uuid=global_id,
                    type=types[global_id],
                    source=source,
                    properties={
                        "is_proxy": True,
                        "remote-type": remote.type,
                        "remote-source": remote.source,
                    },
                )
            )
        adapter.insert_nodes(nodes)
        proxy_count += len(nodes)

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
        endpoint_ids = [
            endpoint
            for relationship in batch
            for endpoint in (relationship.from_uuid, relationship.to_uuid)
        ]
        endpoint_types = endpoint_directory.get_types(endpoint_ids)
        adapter.insert_relationships(batch, endpoint_types)
        relationship_count += len(batch)

    _progress(source, "building traversal indexes and analyzing tables")
    adapter.finalize(inspection.node_types, inspection.relationship_types)
    _progress(source, "load completed")
    return node_count, proxy_count, relationship_count


def inspect_gpu_projection(
    input_graph: str | Path,
    endpoint_directory: EndpointDirectory,
    routing_layout: BackendLayout,
    *,
    device_id: int = 0,
) -> dict[str, int]:
    del routing_layout
    return endpoint_directory.projection_stats(input_graph, device_id=device_id)

def load_graph(
    input_graph: str | Path,
    backend_config: str | Path,
    *,
    reset: bool = False,
    batch_size: int = 20_000,
    load_workers: int = 4,
    only_sources: Iterable[str] | None = None,
    resolved_output: str | Path | None = None,
    id_map_output: str | Path | None = None,
    connection_overrides: Mapping[str, BackendConnection] | None = None,
) -> dict[str, dict[str, int | str]]:
    if batch_size <= 0:
        raise ValueError("batch_size must be positive")
    if load_workers <= 0:
        raise ValueError("load_workers must be positive")
    del resolved_output, id_map_output  # stable payload IDs require neither sidecar nor graph rewrite.

    connections = read_backend_connections(backend_config)
    if connection_overrides:
        connections.update(connection_overrides)
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
    if metadata.get("neo4jNativeIdsResolved"):
        raise ValueError(
            "This graph was resolved with deprecated Neo4j id(n)/id(r) values. "
            "Regenerate it with the stable 48-bit payload format before loading."
        )

    endpoint_directory = build_endpoint_directory(
        working_graph,
        routing_layout,
        progress_source="orchestrator",
    )
    try:
        if "Neo4J" in selected:
            if not reset:
                result["Neo4J"] = {
                    "nodes": 0,
                    "proxies": 0,
                    "relationships": 0,
                    "reused_existing_stable_ids": 1,
                }
                _progress(
                    "Neo4J",
                    "--no-reset selected; reusing existing database because GlobalId payload IDs are stable",
                )
            else:
                if "Neo4J" not in connections:
                    raise ValueError("Neo4J is selected but has no configured connection")
                _, neo_result = _load_neo4j_fresh(
                    input_graph=working_graph,
                    adapter=Neo4jHttpAdapter(connections["Neo4J"]),
                    routing_layout=routing_layout,
                    batch_size=batch_size,
                    endpoint_directory=endpoint_directory,
                )
                result["Neo4J"] = neo_result
                _progress(
                    "orchestrator",
                    "Neo4J finished with stable payload IDs; no old/new ID map or resolved graph rewrite was needed",
                )

        if "ArangoDB" in selected:
            if "ArangoDB" not in connections:
                raise ValueError("ArangoDB is selected but has no configured connection")
            adapter = ArangoHttpAdapter(connections["ArangoDB"], import_workers=load_workers)
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

        if "PostgreSQL" in selected:
            if "PostgreSQL" not in connections:
                raise ValueError("PostgreSQL is selected but has no configured connection")
            nodes, proxies, relationships = load_postgresql_source(
                input_graph=working_graph,
                adapter=PostgreSqlAdapter(connections["PostgreSQL"]),
                endpoint_directory=endpoint_directory,
                routing_layout=routing_layout,
                reset=reset,
                batch_size=batch_size,
            )
            result["PostgreSQL"] = {
                "nodes": nodes,
                "proxies": proxies,
                "relationships": relationships,
            }

        for backend_name in sorted(selected):
            if backend_name == "GPU" or backend_name.startswith("GPU:"):
                device_id = 0 if backend_name == "GPU" else int(backend_name.split(":", 1)[1])
                result[f"GPU:{device_id}"] = inspect_gpu_projection(
                    working_graph, endpoint_directory, routing_layout, device_id=device_id
                )
            elif backend_name in {"Redis", "MMap"}:
                result[backend_name] = {"prepared_by_generation": 1}
        return result
    finally:
        index_path = endpoint_directory.path
        endpoint_directory.close()
        # This is a load-time scratch index, not part of the prepared dataset.
        # Keeping it would consume substantial disk at SF100/SF1000.
        index_path.unlink(missing_ok=True)



def sync_cache_residency(
    input_graph: str | Path,
    backend_config: str | Path,
    *,
    batch_size: int = 20_000,
    only_sources: Iterable[str] | None = None,
    connection_overrides: Mapping[str, BackendConnection] | None = None,
) -> dict[str, dict[str, int]]:
    """Persist the active graph's cache_residency node attribute without reloading edges.

    This is intended after ``cache reconfigure`` when Neo4j/ArangoDB are already
    loaded. It is streaming/batched and O(batch_size) host memory.
    """
    if batch_size <= 0:
        raise ValueError("batch_size must be positive")
    connections = read_backend_connections(backend_config)
    if connection_overrides:
        connections.update(connection_overrides)
    enabled = read_enabled_backend_names(backend_config)
    selected = set(only_sources or ("Neo4J", "ArangoDB", "PostgreSQL")) & enabled
    routing_layout = read_id_routing_layout(backend_config)
    validate_graph_routing_layout(input_graph, routing_layout)
    graph = Path(input_graph).expanduser().resolve()
    result: dict[str, dict[str, int]] = {}

    if "Neo4J" in selected:
        adapter = Neo4jHttpAdapter(connections["Neo4J"])
        count = 0
        nodes = (
            node for node in open_graph_reader(graph).iter_nodes()
            if routing_layout.decode_source(node.uuid) == "Neo4J"
        )
        for batch in progress_batched(
            nodes, batch_size, source="Neo4J", stage="sync cache_residency"
        ):
            adapter.update_cache_residency(batch, routing_layout)
            count += len(batch)
        result["Neo4J"] = {"nodes_updated": count}

    if "ArangoDB" in selected:
        adapter = ArangoHttpAdapter(connections["ArangoDB"], import_workers=1)
        count = 0
        nodes = (
            node for node in open_graph_reader(graph).iter_nodes()
            if routing_layout.decode_source(node.uuid) == "ArangoDB"
        )
        for batch in progress_batched(
            nodes, batch_size, source="ArangoDB", stage="sync cache_residency"
        ):
            adapter.update_cache_residency(batch)
            count += len(batch)
        result["ArangoDB"] = {"nodes_updated": count}

    if "PostgreSQL" in selected:
        adapter = PostgreSqlAdapter(connections["PostgreSQL"])
        count = 0
        nodes = (
            node for node in open_graph_reader(graph).iter_nodes()
            if routing_layout.decode_source(node.uuid) == "PostgreSQL"
        )
        for batch in progress_batched(
            nodes, batch_size, source="PostgreSQL", stage="sync cache_residency"
        ):
            adapter.update_cache_residency(batch)
            count += len(batch)
        result["PostgreSQL"] = {"nodes_updated": count}

    return result
