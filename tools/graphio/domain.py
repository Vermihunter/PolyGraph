from __future__ import annotations

import json
import os
import random
import re
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

from .id_codec import (
    DEFAULT_ROUTING_BACKENDS,
    BackendLayout,
    deterministic_entity_uuid,
    deterministic_routed_entity_uuid,
    parse_global_id,
    read_native_node_id_map,
    resolve_native_node_id,
    gpu_location_bit,
)
from .model import NodeRecord, RelationshipRecord
from .reader import open_graph_reader
from .writer import GraphJsonLinesWriter


@dataclass(frozen=True)
class DomainPaths:
    manifest: Path
    schema: Path
    data: Path
    backends: Path
    observability: Path
    source_data: Path | None = None
    native_id_map: Path | None = None
    cache_config: Path | None = None
    cache_plan: Path | None = None
    gpu_distribution_config: Path | None = None
    gpu_storage_plan: Path | None = None


@dataclass(frozen=True)
class DomainConfig:
    domain_id: str
    name: str
    description: str
    paths: DomainPaths
    graph_version: int
    verbose: bool
    start_uuid: str
    max_depth: int
    direction: str


def _resolve_relative(base: Path, value: str) -> Path:
    candidate = Path(os.path.expandvars(os.path.expanduser(value)))
    return candidate.resolve() if candidate.is_absolute() else (base / candidate).resolve()


def _portable_path(target: Path, base: Path) -> str:
    target = target.resolve()
    base = base.resolve()
    try:
        return os.path.relpath(target, base)
    except ValueError:
        return str(target)


def _default_observability_config() -> Path:
    return Path(__file__).resolve().parents[2] / "config" / "observability" / "observability.json"


def materialize_graph_jsonl(
    input_path: str | Path,
    output_path: str | Path,
    *,
    metadata: dict[str, Any] | None = None,
    schema_path: str | Path | None = None,
    placement_strategy: str = "schema",
    routing_backends: Iterable[str] | None = None,
    reroute: bool = False,
    migrate_missing_routing: bool = False,
) -> tuple[int, int]:
    """Convert a supported graph input into canonical PolyGraph JSONL.

    Legacy inputs without ``idRouting`` metadata can be migrated by supplying a
    graph-shape schema and setting ``migrate_missing_routing``. ``reroute`` forces
    the same migration for an already routed graph.
    """
    source = Path(input_path).expanduser().resolve()
    output = Path(output_path).expanduser().resolve()
    if not source.is_file():
        raise FileNotFoundError(f"Graph input does not exist: {source}")

    output.parent.mkdir(parents=True, exist_ok=True)
    reader = open_graph_reader(source)
    input_metadata = reader.metadata()
    has_routing_metadata = bool(input_metadata.get("idRouting"))
    should_reroute = reroute or (migrate_missing_routing and not has_routing_metadata)

    if should_reroute:
        if schema_path is None:
            raise ValueError("ID migration requires a graph-shape schema")
        return _reroute_graph_jsonl(
            reader,
            source,
            output,
            schema_path=Path(schema_path).expanduser().resolve(),
            placement_strategy=placement_strategy,
            routing_backends=routing_backends,
            metadata=metadata,
            input_metadata=input_metadata,
            legacy_input=not has_routing_metadata,
        )

    merged_metadata = {**input_metadata, "source": str(source), **(metadata or {})}
    if "idRouting" not in merged_metadata:
        raise ValueError(
            "Input graph has no idRouting metadata. Supply a schema to migrate legacy IDs "
            "or regenerate the graph in MSB-routed uint64 format."
        )

    temporary = output.with_name(f".{output.name}.tmp")
    node_count = 0
    relationship_count = 0
    try:
        with GraphJsonLinesWriter(temporary, metadata=merged_metadata) as writer:
            for node in reader.iter_nodes():
                writer.write_node(node)
                node_count += 1
            for relationship in reader.iter_relationships():
                writer.write_relationship(relationship)
                relationship_count += 1
        temporary.replace(output)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise

    return node_count, relationship_count


def write_domain_manifest(
    path: str | Path,
    *,
    domain_id: str,
    name: str,
    description: str,
    schema: str | Path,
    data: str | Path,
    backends: str | Path,
    start_uuid: int | str,
    observability: str | Path | None = None,
    cache_config: str | Path | None = None,
    cache_plan: str | Path | None = None,
    gpu_distribution_config: str | Path | None = None,
    gpu_storage_plan: str | Path | None = None,
    graph_version: int = 1,
    verbose: bool = False,
    max_depth: int = 7,
    direction: str = "undirected",
) -> Path:
    output = Path(path).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    base = output.parent
    payload = {
        "format": "polygraph.domain",
        "version": 1,
        "id": domain_id,
        "name": name,
        "description": description,
        "paths": {
            "schema": _portable_path(Path(schema), base),
            "data": _portable_path(Path(data), base),
            "backendConfig": _portable_path(Path(backends), base),
            "observabilityConfig": _portable_path(
                Path(observability) if observability is not None else _default_observability_config(),
                base,
            ),
        },
        "runtime": {
            "graphVersion": int(graph_version),
            "verbose": bool(verbose),
        },
        "defaults": {
            "bfs": {
                "startUuid": str(start_uuid),
                "maxDepth": int(max_depth),
                "direction": direction,
            }
        },
    }
    if cache_config is not None:
        payload["paths"]["cacheConfig"] = _portable_path(Path(cache_config), base)
    if cache_plan is not None:
        payload["paths"]["cachePlan"] = _portable_path(Path(cache_plan), base)
    if gpu_distribution_config is not None:
        payload["paths"]["gpuDistributionConfig"] = _portable_path(
            Path(gpu_distribution_config), base
        )
    if gpu_storage_plan is not None:
        payload["paths"]["gpuStoragePlan"] = _portable_path(Path(gpu_storage_plan), base)
    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return output


def load_domain_manifest(path: str | Path) -> DomainConfig:
    manifest = Path(path).expanduser().resolve()
    raw = json.loads(manifest.read_text(encoding="utf-8"))
    if raw.get("format") != "polygraph.domain" or raw.get("version") != 1:
        raise ValueError(f"Unsupported domain manifest: {manifest}")
    paths = raw.get("paths", {})
    runtime = raw.get("runtime", {})
    bfs = raw.get("defaults", {}).get("bfs", {})
    base = manifest.parent
    return DomainConfig(
        domain_id=str(raw.get("id", manifest.parent.name)),
        name=str(raw.get("name", raw.get("id", manifest.parent.name))),
        description=str(raw.get("description", "")),
        paths=DomainPaths(
            manifest=manifest,
            schema=_resolve_relative(base, str(paths["schema"])),
            data=_resolve_relative(base, str(paths["data"])),
            backends=_resolve_relative(base, str(paths["backendConfig"])),
            observability=_resolve_relative(
                base,
                str(paths.get("observabilityConfig", _default_observability_config())),
            ),
            source_data=(
                _resolve_relative(base, str(paths["sourceData"]))
                if paths.get("sourceData")
                else None
            ),
            native_id_map=(
                _resolve_relative(base, str(paths["nativeIdMap"]))
                if paths.get("nativeIdMap")
                else None
            ),
            cache_config=(
                _resolve_relative(base, str(paths["cacheConfig"]))
                if paths.get("cacheConfig")
                else None
            ),
            cache_plan=(
                _resolve_relative(base, str(paths["cachePlan"]))
                if paths.get("cachePlan")
                else None
            ),
            gpu_distribution_config=(
                _resolve_relative(base, str(paths["gpuDistributionConfig"]))
                if paths.get("gpuDistributionConfig")
                else None
            ),
            gpu_storage_plan=(
                _resolve_relative(base, str(paths["gpuStoragePlan"]))
                if paths.get("gpuStoragePlan")
                else None
            ),
        ),
        graph_version=int(runtime.get("graphVersion", 1)),
        verbose=bool(runtime.get("verbose", False)),
        start_uuid=str(bfs["startUuid"]),
        max_depth=int(bfs.get("maxDepth", 7)),
        direction=str(bfs.get("direction", "undirected")),
    )



def resolve_domain_node_id(domain: DomainConfig, value: int | str) -> int:
    """Resolve an original generated node ID to the currently loaded Neo4j localId-based global ID."""
    mapping = read_native_node_id_map(domain.paths.native_id_map)
    return resolve_native_node_id(value, mapping)


def record_native_id_resolution(
    domain: DomainConfig,
    *,
    resolved_graph: str | Path,
    native_id_map: str | Path,
) -> DomainConfig:
    """Make the domain manifest point at the graph matching the current Neo4j localId allocation."""
    manifest = domain.paths.manifest
    raw = json.loads(manifest.read_text(encoding="utf-8"))
    paths = raw.setdefault("paths", {})
    base = manifest.parent
    source_data = domain.paths.source_data or domain.paths.data
    resolved = Path(resolved_graph).expanduser().resolve()
    mapping_path = Path(native_id_map).expanduser().resolve()
    mapping = read_native_node_id_map(mapping_path)

    paths["sourceData"] = _portable_path(source_data, base)
    paths["data"] = _portable_path(resolved, base)
    paths["nativeIdMap"] = _portable_path(mapping_path, base)
    bfs = raw.setdefault("defaults", {}).setdefault("bfs", {})
    source_start = bfs.setdefault("sourceStartUuid", bfs["startUuid"])
    bfs["startUuid"] = str(resolve_native_node_id(source_start, mapping))
    manifest.write_text(json.dumps(raw, indent=2) + "\n", encoding="utf-8")
    return load_domain_manifest(manifest)

def discover_domain_manifests(data_root: str | Path) -> list[Path]:
    root = Path(data_root).expanduser().resolve()
    if not root.exists():
        return []
    return sorted(root.rglob("domain.json"))


def resolve_domain(reference: str | Path, data_root: str | Path) -> DomainConfig:
    candidate = Path(reference).expanduser()
    if candidate.is_file():
        return load_domain_manifest(candidate)

    matches: list[DomainConfig] = []
    for manifest in discover_domain_manifests(data_root):
        try:
            domain = load_domain_manifest(manifest)
        except (KeyError, TypeError, ValueError, json.JSONDecodeError):
            continue
        if domain.domain_id == str(reference) or manifest.parent.name == str(reference):
            matches.append(domain)
    if not matches:
        raise FileNotFoundError(f"Unknown domain '{reference}'. Run 'python3 tools/polygraph.py list'.")
    if len(matches) > 1:
        rendered = "\n".join(f"  {item.paths.manifest}" for item in matches)
        raise ValueError(f"Domain reference '{reference}' is ambiguous:\n{rendered}")
    return matches[0]


def sanitize_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_]", "_", value)
    if not cleaned:
        cleaned = "X"
    if cleaned[0].isdigit():
        cleaned = f"X_{cleaned}"
    return cleaned


def _config_bytes(value: Any) -> int:
    if value is None:
        return 0
    if isinstance(value, (int, float)):
        return int(value)
    text = str(value).strip().lower().replace(" ", "")
    units = {"kib": 1024, "mib": 1024**2, "gib": 1024**3, "tib": 1024**4,
             "kb": 1000, "mb": 1000**2, "gb": 1000**3, "tb": 1000**4, "b": 1}
    for suffix in sorted(units, key=len, reverse=True):
        if text.endswith(suffix):
            return int(float(text[:-len(suffix)]) * units[suffix])
    return int(text)


def _backend_entry(source: str, enabled: bool, arango_database: str) -> dict[str, Any]:
    if source == "Neo4J":
        return {
            "id": "Neo4J",
            "kind": "Neo4j",
            "enabled": enabled,
            "name": "Neo4j Primary",
            "priority": 50,
            "connection": {
                "endpoint": "http://localhost:7474/db/neo4j/tx/commit",
                "database": "neo4j",
                "credentials": {"username": "neo4j", "password": "password"},
            },
            "capabilities": {"batch_neighbors": True, "native_bfs": False, "property_filters": True},
            "coverage": {"mode": "id-routing", "fresh": True, "complete_for_requested_subgraph": False},
            "scheduler": {
                "max_in_flight": 64,
                "max_queued": 4096,
                "max_queued_per_query": 512,
                "policy": "priority-weighted-fair",
                "priority_aging_ms": 5000,
            },
        }
    if source == "ArangoDB":
        return {
            "id": "ArangoDB",
            "kind": "ArangoDB",
            "enabled": enabled,
            "name": "ArangoDB Primary",
            "priority": 50,
            "connection": {
                "endpoint": f"http://localhost:8529/_db/{arango_database}/_api/cursor",
                "database": arango_database,
                "credentials": {"username": "root", "password": "rootpassword"},
            },
            "capabilities": {"batch_neighbors": True, "native_bfs": False, "gpu_execution": False},
            "coverage": {"mode": "id-routing", "fresh": True, "complete_for_requested_subgraph": False},
            "scheduler": {
                "max_in_flight": 128,
                "max_queued": 8192,
                "max_queued_per_query": 1024,
                "policy": "priority-weighted-fair",
                "priority_aging_ms": 5000,
            },
        }
    if source == "PostgreSQL":
        return {
            "id": "PostgreSQL",
            "kind": "PostgreSQL",
            "enabled": enabled,
            "name": "PostgreSQL Primary",
            "priority": 50,
            "connection": {
                "endpoint": "127.0.0.1:5432",
                "database": "polygraph",
                "credentials": {"username": "polygraph", "password": "polygraph"},
            },
            "server_side_candidate_deduplication": False,
            "capabilities": {
                "batch_neighbors": True,
                "native_bfs": False,
                "property_filters": False,
                "gpu_execution": False,
                "replica_parallelism": False,
            },
            "coverage": {"mode": "id-routing", "fresh": True, "complete_for_requested_subgraph": False},
        }
    if source == "GPU":
        return {
            "id": "GPU",
            "kind": "CuGraph",
            "enabled": enabled,
            "name": "GPU projection",
            "priority": 1,
            "connection": {"endpoint": "", "database": ""},
            "capabilities": {
                "batch_neighbors": True,
                "native_bfs": False,
                "property_filters": False,
                "gpu_execution": True,
            },
            "coverage": {"mode": "id-routing", "fresh": True, "complete_for_requested_subgraph": False},
        }
    raise ValueError(f"Unsupported backend source: {source}")


def write_backend_config_for_sources(
    sources: Iterable[str],
    output_path: str | Path,
    *,
    arango_database: str,
    cache_config: dict[str, Any] | None = None,
    cache_root: str | Path | None = None,
    gpu_distribution_config: dict[str, Any] | None = None,
    gpu_storage_plan: str | Path | None = None,
) -> Path:
    requested = tuple(str(source) for source in sources)
    routing_layout = BackendLayout(requested)
    used = set(routing_layout.backends)
    unknown = used - {"Neo4J", "ArangoDB", "PostgreSQL", "GPU"}
    if unknown:
        raise ValueError(f"Unsupported authoritative backend sources: {sorted(unknown)}")
    output = Path(output_path).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    # Cache backends are no longer part of the static domain configuration.
    # cache_config/cache_root remain accepted for source compatibility with
    # callers from older prepared domains, but cache-plan.json injects those
    # runtime backends independently.
    _ = cache_config, cache_root

    distribution = gpu_distribution_config or {}
    priorities = dict(distribution.get("priorities", {}))
    backends = [
        _backend_entry(source, source in used, arango_database)
        for source in ("Neo4J", "ArangoDB", "PostgreSQL")
    ]

    # Without a materialized storage plan keep the legacy GPU:0 behavior so old
    # callers still get a runnable backend config. New LDBC preparation always
    # materializes gpu-storage-plan.json first and runtime injects its physical
    # GPU:N backend set from that plan.
    if "GPU" in used and gpu_storage_plan is None:
        configured_gpus = [
            gpu
            for gpu in distribution.get("gpus", [])
            if gpu.get("enabled", True)
        ]
        if not configured_gpus:
            configured_gpus = [
                {
                    "device_id": 0,
                    "name": "GPU 0 authoritative storage",
                    "priority": priorities.get("GPU", 0),
                    "regions": [{"name": "all", "policy": {"kind": "all"}}],
                }
            ]
        for gpu in configured_gpus:
            device_id = int(gpu["device_id"])
            gpu_location_bit(device_id)
            backends.append({
                "id": f"GPU:{device_id}",
                "kind": "CuGraph",
                "enabled": True,
                "name": str(gpu.get("name", f"GPU {device_id} authoritative storage")),
                "priority": int(
                    gpu.get(
                        "priority",
                        priorities.get(f"GPU:{device_id}", priorities.get("GPU", 0)),
                    )
                ),
                "device_id": device_id,
                "memory_limit_bytes": _config_bytes(
                    gpu.get("memory_limit_bytes", gpu.get("memory", 0))
                ),
                "relation_limit": int(
                    gpu.get("relation_limit", gpu.get("relations", 0)) or 0
                ),
                "regions": gpu.get("regions", []),
                "storage_role": "authoritative",
                "connection": {"endpoint": "", "database": ""},
                "capabilities": {
                    "batch_neighbors": True,
                    "native_bfs": False,
                    "property_filters": False,
                    "gpu_execution": True,
                    "replica_parallelism": False,
                },
                "coverage": {
                    "mode": "id-routing",
                    "fresh": True,
                    "complete_for_requested_subgraph": False,
                },
            })

    payload = {
        "version": 2,
        "idRouting": list(routing_layout.backends),
        "idLocationEncoding": "bitmask-msb-v1",
        "backends": backends,
    }
    if gpu_storage_plan is not None:
        payload["gpuStoragePlan"] = _portable_path(Path(gpu_storage_plan), output.parent)

    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return output


def write_backend_config_from_schema(
    schema_path: str | Path,
    output_path: str | Path,
    *,
    arango_database: str,
) -> Path:
    schema = json.loads(Path(schema_path).read_text(encoding="utf-8"))
    used = {
        str(entry["source"])
        for section in ("nodeTypes", "relations")
        for entry in schema.get(section, [])
        if "source" in entry
    }
    if not used:
        raise ValueError(
            "Schema contains no storage sources; call write_backend_config_for_sources with generator placement"
        )
    return write_backend_config_for_sources(
        BackendLayout.from_sources(used).backends, output_path, arango_database=arango_database
    )


def _sample_property_value(property_spec: dict[str, Any], index: int) -> Any:
    if "sampleValue" in property_spec:
        sample = property_spec["sampleValue"]
    else:
        type_name = str(property_spec.get("type", "string"))
        sample = {
            "integer": index + 1,
            "float": float(index + 1),
            "boolean": index % 2 == 0,
            "date": "2026-01-01",
            "datetime": "2026-01-01T00:00:00",
        }.get(type_name, f"sample-{index + 1}")

    if index == 0:
        return sample
    if isinstance(sample, bool):
        return sample if index % 2 == 0 else not sample
    if isinstance(sample, int):
        return sample + index
    if isinstance(sample, float):
        return sample + float(index)
    if isinstance(sample, str) and property_spec.get("type") == "string":
        return f"{sample}-{index + 1}"
    return sample


def _schema_routing_layout(
    schema: dict[str, Any],
    routing_backends: Iterable[str] | None,
    *,
    require_type_sources: bool,
) -> BackendLayout:
    configured = routing_backends
    if configured is None:
        configured = schema.get("idRouting") or schema.get("storageBackends")

    entries = [
        entry
        for section in ("nodeTypes", "relations")
        for entry in schema.get(section, [])
    ]
    schema_sources = {str(entry["source"]) for entry in entries if entry.get("source")}

    if require_type_sources:
        missing = [str(entry.get("name", "<unnamed>")) for entry in entries if not entry.get("source")]
        if missing:
            raise ValueError(
                "Schema placement requires a source for every node/relation type; "
                f"missing: {', '.join(missing[:10])}"
            )

    if configured is not None:
        layout = BackendLayout(tuple(str(value) for value in configured))
        for source in schema_sources:
            layout.code_for(source)
        return layout

    if schema_sources:
        return BackendLayout.from_sources(schema_sources)
    if require_type_sources:
        raise ValueError("Schema contains no backend source assignments")
    return BackendLayout(DEFAULT_ROUTING_BACKENDS)


def _schema_type_source_maps(schema: dict[str, Any]) -> tuple[dict[str, str], dict[str, str]]:
    node_sources = {
        str(entry["name"]): str(entry["source"])
        for entry in schema.get("nodeTypes", [])
        if entry.get("source")
    }
    relationship_sources = {
        str(entry["name"]): str(entry["source"])
        for entry in schema.get("relations", [])
        if entry.get("source")
    }
    return node_sources, relationship_sources


def _placement_source(
    *,
    entity_kind: str,
    entity_type: str,
    legacy_uuid: str | int,
    placement_strategy: str,
    routing_layout: BackendLayout,
    schema_sources: dict[str, str],
) -> str:
    if placement_strategy == "identity-hash":
        return routing_layout.source_for_identity(
            f"{entity_kind}:{entity_type}:{legacy_uuid}"
        )
    try:
        configured_source = schema_sources[entity_type]
    except KeyError as exc:
        raise ValueError(
            f"Schema placement has no source for {entity_kind} type '{entity_type}'"
        ) from exc
    return routing_layout.source_for_code(routing_layout.code_for(configured_source))


def _lookup_migrated_uuid(
    connection: sqlite3.Connection,
    legacy_uuid: str | int,
    *,
    relationship_uuid: str,
    endpoint_name: str,
) -> str:
    row = connection.execute(
        "SELECT migrated_uuid FROM node_uuid_map WHERE legacy_uuid = ?",
        (str(legacy_uuid),),
    ).fetchone()
    if row is None:
        raise ValueError(
            f"Relationship {relationship_uuid} references unknown {endpoint_name} "
            f"node UUID {legacy_uuid}"
        )
    return int(row[0])


def _reroute_graph_jsonl(
    reader: Any,
    source: Path,
    output: Path,
    *,
    schema_path: Path,
    placement_strategy: str,
    routing_backends: Iterable[str] | None,
    metadata: dict[str, Any] | None,
    input_metadata: dict[str, Any],
    legacy_input: bool,
) -> tuple[int, int]:
    if placement_strategy not in {"schema", "identity-hash"}:
        raise ValueError(f"Unsupported placement strategy: {placement_strategy}")
    if not schema_path.is_file():
        raise FileNotFoundError(f"Graph-shape schema does not exist: {schema_path}")

    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    use_schema_placement = placement_strategy == "schema"
    routing_layout = _schema_routing_layout(
        schema,
        routing_backends,
        require_type_sources=use_schema_placement,
    )
    node_sources, relationship_sources = _schema_type_source_maps(schema)

    merged_metadata = {
        **input_metadata,
        "source": str(source),
        **(metadata or {}),
        "idRoutingStrategy": (
            "schema-type-placement-v1" if use_schema_placement else "identity-hash-v1"
        ),
        **routing_layout.metadata(),
        "uuidMigration": {
            "version": 1,
            "legacyInput": legacy_input,
            "placementStrategy": placement_strategy,
            "schema": str(schema_path),
        },
    }

    temporary = output.with_name(f".{output.name}.tmp")
    mapping_path = output.with_name(f".{output.name}.uuid-map.sqlite3")
    temporary.unlink(missing_ok=True)
    mapping_path.unlink(missing_ok=True)
    node_count = 0
    relationship_count = 0
    connection = sqlite3.connect(mapping_path)
    try:
        connection.execute(
            "CREATE TABLE node_uuid_map ("
            "legacy_uuid TEXT PRIMARY KEY, migrated_uuid TEXT NOT NULL UNIQUE)"
        )
        with GraphJsonLinesWriter(temporary, metadata=merged_metadata) as writer:
            for node in reader.iter_nodes():
                source_name = _placement_source(
                    entity_kind="node",
                    entity_type=node.type,
                    legacy_uuid=node.uuid,
                    placement_strategy=placement_strategy,
                    routing_layout=routing_layout,
                    schema_sources=node_sources,
                )
                migrated_uuid = deterministic_entity_uuid(
                    source_name,
                    "node",
                    node.type,
                    node.uuid,
                    routing_layout,
                )
                try:
                    connection.execute(
                        "INSERT INTO node_uuid_map(legacy_uuid, migrated_uuid) VALUES (?, ?)",
                        (str(node.uuid), str(migrated_uuid)),
                    )
                except sqlite3.IntegrityError as exc:
                    raise ValueError(
                        f"Duplicate or colliding node UUID during migration: {node.uuid}"
                    ) from exc
                writer.write_node(
                    NodeRecord(
                        uuid=migrated_uuid,
                        type=node.type,
                        source=source_name,
                        properties=node.properties,
                    )
                )
                node_count += 1

            connection.commit()
            for relationship in reader.iter_relationships():
                source_name = _placement_source(
                    entity_kind="relationship",
                    entity_type=relationship.type,
                    legacy_uuid=relationship.uuid,
                    placement_strategy=placement_strategy,
                    routing_layout=routing_layout,
                    schema_sources=relationship_sources,
                )
                migrated_uuid = deterministic_entity_uuid(
                    source_name,
                    "relationship",
                    relationship.type,
                    relationship.uuid,
                    routing_layout,
                )
                writer.write_relationship(
                    RelationshipRecord(
                        uuid=migrated_uuid,
                        type=relationship.type,
                        source=source_name,
                        from_uuid=_lookup_migrated_uuid(
                            connection,
                            relationship.from_uuid,
                            relationship_uuid=relationship.uuid,
                            endpoint_name="source",
                        ),
                        to_uuid=_lookup_migrated_uuid(
                            connection,
                            relationship.to_uuid,
                            relationship_uuid=relationship.uuid,
                            endpoint_name="target",
                        ),
                        properties=relationship.properties,
                    )
                )
                relationship_count += 1

        temporary.replace(output)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    finally:
        connection.close()
        mapping_path.unlink(missing_ok=True)

    return node_count, relationship_count


def generate_sample_graph(
    schema_path: str | Path,
    output_path: str | Path,
    *,
    nodes_per_type: int = 3,
    seed: int = 42,
    placement_strategy: str = "schema",
    routing_backends: Iterable[str] | None = None,
) -> tuple[int, int]:
    if nodes_per_type <= 0:
        raise ValueError("nodes_per_type must be positive")
    if placement_strategy not in {"schema", "identity-hash"}:
        raise ValueError(f"Unsupported placement strategy: {placement_strategy}")

    schema = json.loads(Path(schema_path).read_text(encoding="utf-8"))
    rng = random.Random(seed)
    use_schema_placement = placement_strategy == "schema"
    routing_layout = _schema_routing_layout(
        schema,
        routing_backends,
        require_type_sources=use_schema_placement,
    )
    nodes_by_type: dict[str, list[NodeRecord]] = {}
    nodes: list[NodeRecord] = []

    for node_type in schema.get("nodeTypes", []):
        type_name = str(node_type["name"])
        type_source = (
            routing_layout.source_for_code(routing_layout.code_for(str(node_type["source"])))
            if use_schema_placement
            else None
        )
        generated: list[NodeRecord] = []
        for index in range(nodes_per_type):
            properties = {
                str(prop["name"]): _sample_property_value(prop, index)
                for prop in node_type.get("properties", [])
            }
            if type_source is None:
                node_uuid, source = deterministic_routed_entity_uuid(
                    "node", type_name, index + 1, routing_layout
                )
            else:
                source = type_source
                node_uuid = deterministic_entity_uuid(
                    source, "node", type_name, index + 1, routing_layout
                )
            node = NodeRecord(
                uuid=node_uuid,
                type=type_name,
                source=source,
                properties=properties,
            )
            generated.append(node)
            nodes.append(node)
        nodes_by_type[type_name] = generated

    relationships: list[RelationshipRecord] = []
    for relation in schema.get("relations", []):
        relation_type = str(relation["name"])
        from_type = str(relation["from"])
        to_type = str(relation["to"])
        type_source = (
            routing_layout.source_for_code(routing_layout.code_for(str(relation["source"])))
            if use_schema_placement
            else None
        )
        if from_type not in nodes_by_type or to_type not in nodes_by_type:
            raise ValueError(f"Relation {relation_type} references unknown types {from_type}->{to_type}")
        for index in range(nodes_per_type):
            source_node = nodes_by_type[from_type][index % len(nodes_by_type[from_type])]
            target_index = index
            if from_type == to_type and nodes_per_type > 1:
                target_index = (index + 1) % nodes_per_type
            target_node = nodes_by_type[to_type][target_index % len(nodes_by_type[to_type])]
            properties = {
                str(prop["name"]): _sample_property_value(prop, index)
                for prop in relation.get("properties", [])
            }
            identity = f"{source_node.uuid}:{target_node.uuid}:{index}:{rng.randrange(1 << 30)}"
            if type_source is None:
                relationship_uuid, source = deterministic_routed_entity_uuid(
                    "relationship", relation_type, identity, routing_layout
                )
            else:
                source = type_source
                relationship_uuid = deterministic_entity_uuid(
                    source, "relationship", relation_type, identity, routing_layout
                )
            relationships.append(
                RelationshipRecord(
                    uuid=relationship_uuid,
                    type=relation_type,
                    source=source,
                    from_uuid=source_node.uuid,
                    to_uuid=target_node.uuid,
                    properties=properties,
                )
            )

    output = Path(output_path).expanduser().resolve()
    with GraphJsonLinesWriter(
        output,
        {
            "domain": schema.get("domain", schema.get("name", "University")),
            "idRoutingStrategy": (
                "schema-type-placement-v1" if use_schema_placement else "identity-hash-v1"
            ),
            **routing_layout.metadata(),
        },
    ) as writer:
        for node in nodes:
            writer.write_node(node)
        for relationship in relationships:
            writer.write_relationship(relationship)
    return len(nodes), len(relationships)


def find_first_node(graph_path: str | Path, node_type: str | None = None) -> NodeRecord:
    first: NodeRecord | None = None
    for node in open_graph_reader(graph_path).iter_nodes():
        if first is None:
            first = node
        if node_type is None or node.type == node_type:
            return node
    if node_type:
        raise ValueError(f"No node of type '{node_type}' exists in {graph_path}")
    raise ValueError(f"No nodes exist in {graph_path}")


def validate_graph(graph_path: str | Path) -> dict[str, Any]:
    reader = open_graph_reader(graph_path)
    node_ids: set[int] = set()
    node_types: dict[str, int] = {}
    relationship_types: dict[str, int] = {}
    sources: dict[str, int] = {}
    errors: list[str] = []

    metadata = reader.metadata()
    routing_layout: BackendLayout | None = None
    configured_layout = metadata.get("idRouting")
    if not configured_layout:
        errors.append("graph metadata is missing idRouting")
    else:
        try:
            routing_layout = BackendLayout(tuple(str(value) for value in configured_layout))
            declared_bits = metadata.get("idRoutingBits")
            if declared_bits is not None and int(declared_bits) != routing_layout.bit_count:
                errors.append(
                    f"idRoutingBits={declared_bits}, expected {routing_layout.bit_count}"
                )
        except (TypeError, ValueError) as exc:
            errors.append(str(exc))

    for node in reader.iter_nodes():
        if node.uuid in node_ids:
            errors.append(f"duplicate node UUID: {node.uuid}")
        node_ids.add(node.uuid)
        node_types[node.type] = node_types.get(node.type, 0) + 1
        key = f"node:{node.source}"
        sources[key] = sources.get(key, 0) + 1
        if routing_layout is not None:
            try:
                encoded_source = routing_layout.decode_source(node.uuid)
                if encoded_source != node.source:
                    errors.append(
                        f"node {node.uuid} source={node.source} encodes {encoded_source}"
                    )
            except ValueError as exc:
                errors.append(f"node {node.uuid}: {exc}")

    relationship_ids: set[int] = set()
    for relationship in reader.iter_relationships():
        if relationship.uuid in relationship_ids:
            errors.append(f"duplicate relationship UUID: {relationship.uuid}")
        relationship_ids.add(relationship.uuid)
        if relationship.from_uuid not in node_ids:
            errors.append(f"relationship {relationship.uuid} has missing from endpoint {relationship.from_uuid}")
        if relationship.to_uuid not in node_ids:
            errors.append(f"relationship {relationship.uuid} has missing to endpoint {relationship.to_uuid}")
        relationship_types[relationship.type] = relationship_types.get(relationship.type, 0) + 1
        key = f"relationship:{relationship.source}"
        sources[key] = sources.get(key, 0) + 1
        if routing_layout is not None:
            try:
                encoded_source = routing_layout.decode_source(relationship.uuid)
                if encoded_source != relationship.source:
                    errors.append(
                        f"relationship {relationship.uuid} source={relationship.source} encodes {encoded_source}"
                    )
            except ValueError as exc:
                errors.append(f"relationship {relationship.uuid}: {exc}")

    return {
        "graph": str(Path(graph_path).resolve()),
        "valid": not errors,
        "id_routing": list(routing_layout.backends) if routing_layout is not None else [],
        "id_routing_bits": routing_layout.bit_count if routing_layout is not None else None,
        "node_count": len(node_ids),
        "relationship_count": len(relationship_ids),
        "node_types": dict(sorted(node_types.items())),
        "relationship_types": dict(sorted(relationship_types.items())),
        "sources": dict(sorted(sources.items())),
        "errors": errors[:100],
        "error_count": len(errors),
    }


def select_roots(
    graph_path: str | Path,
    *,
    node_type: str,
    count: int = 100,
    seed: int = 42,
) -> list[NodeRecord]:
    if count <= 0:
        raise ValueError("count must be positive")
    rng = random.Random(seed)
    selected: list[NodeRecord] = []
    candidates = 0
    for node in open_graph_reader(graph_path).iter_nodes():
        if node.type != node_type:
            continue
        candidates += 1
        if len(selected) < count:
            selected.append(node)
            continue
        replacement = rng.randrange(candidates)
        if replacement < count:
            selected[replacement] = node
    if not selected:
        raise ValueError(f"No nodes of type '{node_type}' exist in {graph_path}")
    rng.shuffle(selected)
    return selected


def write_roots_csv(path: str | Path, roots: Iterable[NodeRecord]) -> Path:
    import csv

    output = Path(path).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["uuid", "type", "source", "external_id"])
        for node in roots:
            writer.writerow([node.uuid, node.type, node.source, node.properties.get("ldbc_id", "")])
    return output
