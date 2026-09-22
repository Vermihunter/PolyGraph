from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from .common import repo_root, resolve_path


@dataclass(frozen=True)
class Neo4jConfig:
    http_endpoint: str = "http://localhost:7474/db/neo4j/tx/commit"
    bolt_uri: str = "bolt://localhost:7687"
    user: str = "neo4j"
    password: str = "password"


@dataclass(frozen=True)
class ArangoConfig:
    base_url: str = "http://localhost:8529"
    user: str = "root"
    password: str = "rootpassword"
    database: str = "university"


@dataclass(frozen=True)
class PostgreSqlConfig:
    endpoint: str = "127.0.0.1:5432"
    database: str = "polygraph"
    user: str = "polygraph"
    password: str = "polygraph"


@dataclass(frozen=True)
class BfsConfig:
    max_depth: int = 7
    direction: str = "undirected"
    neighbor_batch_size: Optional[int] = None
    neo4j_max_concurrent_requests: int = 4
    arango_max_concurrent_requests: int = 4
    backend_request_retries: int = 2
    backend_retry_delay_ms: int = 100
    allowed_edge_types: str = ""
    allowed_target_node_types: str = ""
    allow_native_pushdown: bool = False
    no_partial_acceleration: bool = False
    no_predecessors: bool = False

    def __post_init__(self) -> None:
        if self.neighbor_batch_size is not None and self.neighbor_batch_size <= 0:
            raise ValueError("bfs.neighbor_batch_size must be greater than zero")
        if self.neo4j_max_concurrent_requests <= 0:
            raise ValueError("bfs.neo4j_max_concurrent_requests must be greater than zero")
        if self.arango_max_concurrent_requests <= 0:
            raise ValueError("bfs.arango_max_concurrent_requests must be greater than zero")
        if self.backend_request_retries < 0:
            raise ValueError("bfs.backend_request_retries must be zero or greater")
        if self.backend_retry_delay_ms < 0:
            raise ValueError("bfs.backend_retry_delay_ms must be zero or greater")


@dataclass(frozen=True)
class ImplementationBackendPaths:
    """Files used when graph_db is benchmarked with all data assigned to one backend."""

    schema: Optional[Path] = None
    data: Optional[Path] = None
    backend_config: Optional[Path] = None


@dataclass(frozen=True)
class PathConfig:
    # Original distributed placement.
    schema: Path
    data: Path
    backend_config: Optional[Path]

    # Executables/scripts.
    graph_db_binary: Path
    loader_script: Path
    reference_bfs_script: Path
    docker_compose: Optional[Path] = None

    # Per-backend graph_db full-placement scenarios.
    implementation_full: dict[str, ImplementationBackendPaths] = field(default_factory=dict)

    # Unresolved graph used whenever the loader recreates Neo4j and must assign
    # a fresh contiguous localId mapping. ``data`` remains the currently active
    # graph used by references and already-loaded scenarios.
    source_data: Optional[Path] = None


@dataclass(frozen=True)
class BenchmarkConfig:
    paths: PathConfig
    neo4j: Neo4jConfig = field(default_factory=Neo4jConfig)
    arango: ArangoConfig = field(default_factory=ArangoConfig)
    postgresql: PostgreSqlConfig = field(default_factory=PostgreSqlConfig)
    bfs: BfsConfig = field(default_factory=BfsConfig)
    batch_size: int = 10000


def _section(root: dict[str, Any], name: str) -> dict[str, Any]:
    value = root.get(name, {})
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f"Config section '{name}' must be an object")
    return value


def _path(value: Any, base: Path) -> Optional[Path]:
    return resolve_path(value, base=base) if value not in (None, "") else None


def _optional_positive_int(value: Any, *, field_name: str) -> Optional[int]:
    if value in (None, ""):
        return None
    parsed = int(value)
    if parsed <= 0:
        raise ValueError(f"'{field_name}' must be greater than zero")
    return parsed


def _default_implementation_paths(base: Path) -> dict[str, ImplementationBackendPaths]:
    return {
        "Neo4J": ImplementationBackendPaths(
            schema=resolve_path("storage_layer/sample-university-domain.neo.graph-shape.json", base=base),
            data=resolve_path("tools/generators/large-university-neo.json", base=base),
            backend_config=resolve_path("config/backends.neo.json", base=base),
        ),
        "ArangoDB": ImplementationBackendPaths(
            schema=resolve_path("storage_layer/sample-university-domain.arango.graph-shape.json", base=base),
            data=resolve_path("tools/generators/large-university-arango.json", base=base),
            backend_config=resolve_path("config/backends.arango.json", base=base),
        ),
        "GPU": ImplementationBackendPaths(
            schema=resolve_path("storage_layer/sample-university-domain.gpu.graph-shape.json", base=base),
            # The project currently has no checked-in GPU-only JSON dataset. When this is None
            # the orchestrator creates a generated copy from paths.data with source="GPU".
            data=None,
            backend_config=resolve_path("config/backends.gpu.json", base=base),
        ),
        "PostgreSQL": ImplementationBackendPaths(
            schema=None,
            data=None,
            backend_config=resolve_path("config/backends/backends.postgresql.json", base=base),
        ),
    }


def _default_path_config(base: Path) -> PathConfig:
    return PathConfig(
        schema=resolve_path("storage_layer/sample-university-domain.graph-shape.json", base=base),
        data=resolve_path("tools/generators/large-university.json", base=base),
        backend_config=resolve_path("config/backends.full.json", base=base),
        graph_db_binary=resolve_path("cmake-build-debug/src/bfs/bfs", base=base),
        loader_script=resolve_path("tools/load_graph.py", base=base),
        reference_bfs_script=resolve_path("tools/algorithms/bfs.py", base=base),
        docker_compose=resolve_path("storage_layer/docker-compose.yml", base=base),
        implementation_full=_default_implementation_paths(base),
    )


def _load_implementation_paths(root: dict[str, Any], defaults: dict[str, ImplementationBackendPaths], base: Path) -> dict[str, ImplementationBackendPaths]:
    """
    Supported JSON shape:

    "implementation_full": {
      "neo4j": {"schema": "...", "data": "...", "backend_config": "..."},
      "arango": {"schema": "...", "data": "...", "backend_config": "..."},
      "gpu":    {"schema": "...", "data": null,  "backend_config": "..."}
    }

    Backward-compatible shortcuts inside "paths" are also accepted:
    schema_neo4j, data_neo4j, backend_config_neo4j, etc.
    """
    canonical = {
        "neo": "Neo4J",
        "neo4j": "Neo4J",
        "Neo4J": "Neo4J",
        "arango": "ArangoDB",
        "arangodb": "ArangoDB",
        "ArangoDB": "ArangoDB",
        "postgres": "PostgreSQL",
        "postgresql": "PostgreSQL",
        "PostgreSQL": "PostgreSQL",
        "gpu": "GPU",
        "cugraph": "GPU",
        "GPU": "GPU",
    }
    result = dict(defaults)
    impl_raw = _section(root, "implementation_full")
    for raw_key, raw_value in impl_raw.items():
        if raw_key not in canonical:
            raise ValueError(f"Unknown implementation_full backend '{raw_key}'")
        if raw_value is None:
            continue
        if not isinstance(raw_value, dict):
            raise ValueError(f"implementation_full.{raw_key} must be an object")
        backend = canonical[raw_key]
        current = result[backend]
        result[backend] = ImplementationBackendPaths(
            schema=_path(raw_value.get("schema", current.schema), base),
            data=_path(raw_value.get("data", current.data), base),
            backend_config=_path(raw_value.get("backend_config", current.backend_config), base),
        )

    paths_raw = _section(root, "paths")
    shortcuts = {
        "Neo4J": ["neo4j", "neo"],
        "ArangoDB": ["arango", "arangodb"],
        "PostgreSQL": ["postgres", "postgresql"],
        "GPU": ["gpu", "cugraph"],
    }
    for backend, names in shortcuts.items():
        current = result[backend]
        values: dict[str, Any] = {}
        for name in names:
            for field_name in ("schema", "data", "backend_config"):
                key = f"{field_name}_{name}"
                if key in paths_raw:
                    values[field_name] = paths_raw[key]
        if values:
            result[backend] = ImplementationBackendPaths(
                schema=_path(values.get("schema", current.schema), base),
                data=_path(values.get("data", current.data), base),
                backend_config=_path(values.get("backend_config", current.backend_config), base),
            )
    return result


def load_config(path: str | Path | None = None) -> BenchmarkConfig:
    """Load a JSON benchmark config. Missing fields get practical project defaults."""
    base = repo_root()
    root: dict[str, Any] = {}
    if path:
        config_path = Path(path).expanduser().resolve()
        with open(config_path, "r", encoding="utf-8") as f:
            root = json.load(f)

    defaults = _default_path_config(base)
    paths_raw = _section(root, "paths")

    paths = PathConfig(
        schema=resolve_path(paths_raw.get("schema", defaults.schema), base=base),
        data=resolve_path(paths_raw.get("data", defaults.data), base=base),
        backend_config=_path(paths_raw.get("backend_config", defaults.backend_config), base),
        graph_db_binary=resolve_path(paths_raw.get("graph_db_binary", defaults.graph_db_binary), base=base),
        loader_script=resolve_path(paths_raw.get("loader_script", defaults.loader_script), base=base),
        reference_bfs_script=resolve_path(paths_raw.get("reference_bfs_script", defaults.reference_bfs_script), base=base),
        docker_compose=_path(paths_raw.get("docker_compose", defaults.docker_compose), base),
        implementation_full=_load_implementation_paths(root, defaults.implementation_full, base),
        source_data=_path(paths_raw.get("source_data"), base),
    )

    neo = _section(root, "neo4j")
    arango = _section(root, "arango")
    postgresql = _section(root, "postgresql")
    bfs = _section(root, "bfs")

    return BenchmarkConfig(
        paths=paths,
        neo4j=Neo4jConfig(
            http_endpoint=neo.get("http_endpoint", Neo4jConfig.http_endpoint),
            bolt_uri=neo.get("bolt_uri", Neo4jConfig.bolt_uri),
            user=neo.get("user", Neo4jConfig.user),
            password=neo.get("password", Neo4jConfig.password),
        ),
        arango=ArangoConfig(
            base_url=arango.get("base_url", ArangoConfig.base_url),
            user=arango.get("user", ArangoConfig.user),
            password=arango.get("password", ArangoConfig.password),
            database=arango.get("database", ArangoConfig.database),
        ),
        postgresql=PostgreSqlConfig(
            endpoint=postgresql.get("endpoint", PostgreSqlConfig.endpoint),
            database=postgresql.get("database", PostgreSqlConfig.database),
            user=postgresql.get("user", PostgreSqlConfig.user),
            password=postgresql.get("password", PostgreSqlConfig.password),
        ),
        bfs=BfsConfig(
            max_depth=int(bfs.get("max_depth", BfsConfig.max_depth)),
            direction=bfs.get("direction", BfsConfig.direction),
            neighbor_batch_size=_optional_positive_int(
                bfs.get("neighbor_batch_size"),
                field_name="bfs.neighbor_batch_size",
            ),
            neo4j_max_concurrent_requests=int(
                bfs.get("neo4j_max_concurrent_requests", BfsConfig.neo4j_max_concurrent_requests)
            ),
            arango_max_concurrent_requests=int(
                bfs.get("arango_max_concurrent_requests", BfsConfig.arango_max_concurrent_requests)
            ),
            backend_request_retries=int(
                bfs.get("backend_request_retries", BfsConfig.backend_request_retries)
            ),
            backend_retry_delay_ms=int(
                bfs.get("backend_retry_delay_ms", BfsConfig.backend_retry_delay_ms)
            ),
            allowed_edge_types=bfs.get("allowed_edge_types", BfsConfig.allowed_edge_types),
            allowed_target_node_types=bfs.get("allowed_target_node_types", BfsConfig.allowed_target_node_types),
            allow_native_pushdown=bool(bfs.get("allow_native_pushdown", BfsConfig.allow_native_pushdown)),
            no_partial_acceleration=bool(bfs.get("no_partial_acceleration", BfsConfig.no_partial_acceleration)),
            no_predecessors=bool(bfs.get("no_predecessors", BfsConfig.no_predecessors)),
        ),
        batch_size=int(root.get("batch_size", 10000)),
    )
