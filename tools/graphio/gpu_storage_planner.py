from __future__ import annotations

import json
import sqlite3
import struct
import sys
from pathlib import Path
from typing import Any

from .cache_planner import (
    _build_gpu_projection_artifact,
    _bytes,
    _index_graph,
    _open_existing_planning_db,
    _open_planning_db,
    _planning_db_counts,
    _select_policy,
    _unsigned_sqlite_id,
)
from .id_codec import BackendLayout, gpu_location_bit
from .reader import open_graph_reader

GPU_OWNER_RECORD = struct.Struct("<QII")


def load_gpu_distribution_config(path: str | Path | None) -> dict[str, Any]:
    """Load physical distribution for authoritative ``source == GPU`` vertices.

    No configuration means the legacy physical layout: every authoritative GPU
    vertex is placed on device 0.  Unlike the old implementation this still gets
    materialized into reusable binary projection artifacts during preparation.
    """
    if path is None:
        return {
            "version": 1,
            "fallback_device_id": 0,
            "gpus": [
                {
                    "device_id": 0,
                    "priority": 0,
                    "regions": [{"name": "all", "policy": {"kind": "all"}}],
                }
            ],
        }

    source = Path(path).expanduser().resolve()
    raw = json.loads(source.read_text(encoding="utf-8"))
    if int(raw.get("version", 1)) != 1:
        raise ValueError(f"Unsupported GPU distribution configuration version in {source}")
    if not isinstance(raw.get("gpus", []), list):
        raise ValueError("GPU distribution configuration field 'gpus' must be an array")
    return raw


def _enabled_gpus(config: dict[str, Any]) -> list[dict[str, Any]]:
    result = [dict(gpu) for gpu in config.get("gpus", []) if gpu.get("enabled", True)]
    seen: set[int] = set()
    for gpu in result:
        if "device_id" not in gpu:
            raise ValueError("GPU distribution entry is missing device_id")
        device_id = int(gpu["device_id"])
        gpu_location_bit(device_id)
        if device_id in seen:
            raise ValueError(f"GPU distribution contains duplicate device_id {device_id}")
        seen.add(device_id)
    return result


def _all_gpu_bits(gpus: list[dict[str, Any]]) -> int:
    mask = 0
    for gpu in gpus:
        mask |= gpu_location_bit(int(gpu["device_id"]))
    return mask


def _write_ownership(db: sqlite3.Connection, output: Path, all_gpu_bits: int) -> dict[str, Any]:
    path = output / "ownership.bin"
    entries = 0
    devices: set[int] = set()
    with path.open("wb") as handle:
        for node_id, mask in db.execute(
            "SELECT id, cache_mask FROM nodes WHERE source='GPU' ORDER BY (id < 0), id"
        ):
            selected = [
                device_id
                for device_id in range(10)
                if int(mask) & gpu_location_bit(device_id)
            ]
            if len(selected) != 1 or (int(mask) & ~all_gpu_bits) != 0:
                raise ValueError(
                    "Every authoritative GPU vertex must have exactly one physical GPU owner; "
                    f"canonical id {_unsigned_sqlite_id(int(node_id))} has mask 0x{int(mask):x}"
                )
            device_id = selected[0]
            handle.write(GPU_OWNER_RECORD.pack(_unsigned_sqlite_id(int(node_id)), device_id, 0))
            entries += 1
            devices.add(device_id)
    return {
        "path": path.name,
        "entries": entries,
        "bytes": path.stat().st_size,
        "record": "<uint64 canonical_id,uint32 device_id,uint32 reserved>",
        "devices": sorted(devices),
    }


def _runtime_gpu_backends(
    config: dict[str, Any], output: Path, artifacts: dict[str, Any]
) -> list[dict[str, Any]]:
    priorities = dict(config.get("priorities", {}))
    configured = {
        int(gpu["device_id"]): gpu
        for gpu in _enabled_gpus(config)
    }
    result: list[dict[str, Any]] = []
    for raw_device_id in sorted(artifacts, key=int):
        device_id = int(raw_device_id)
        gpu = configured[device_id]
        result.append(
            {
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
                "memory_limit_bytes": _bytes(
                    gpu.get("memory_limit_bytes", gpu.get("memory", 0))
                ),
                "relation_limit": int(
                    gpu.get("relation_limit", gpu.get("relations", 0)) or 0
                ),
                "regions": gpu.get("regions", []),
                "projection_artifact": str(
                    Path("gpu") / f"gpu{device_id}" / "manifest.json"
                ),
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
            }
        )
    return result


def plan_gpu_storage(
    graph_path: str | Path,
    distribution_config: str | Path | dict[str, Any] | None,
    *,
    output_dir: str | Path,
    routing_layout: BackendLayout | None = None,
    planning_db_path: str | Path | None = None,
) -> dict[str, Any]:
    """Materialize authoritative GPU placement independently of cache residency."""
    graph = Path(graph_path).expanduser().resolve()
    output = Path(output_dir).expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    config = (
        dict(distribution_config)
        if isinstance(distribution_config, dict)
        else load_gpu_distribution_config(distribution_config)
    )
    metadata = open_graph_reader(graph).metadata()
    _ = routing_layout or BackendLayout(tuple(metadata.get("idRouting", ())))
    gpus = _enabled_gpus(config)

    planning_db = (
        Path(planning_db_path).expanduser().resolve()
        if planning_db_path is not None
        else graph.parent / "cache-plan.sqlite3"
    )
    reuse_index = planning_db.is_file()
    if reuse_index:
        print(f"[gpu-storage] reusing graph/index database {planning_db}", file=sys.stderr, flush=True)
        try:
            db = _open_existing_planning_db(planning_db, graph)
        except (ValueError, sqlite3.DatabaseError):
            planning_db.unlink(missing_ok=True)
            db = _open_planning_db(planning_db)
            reuse_index = False
    else:
        db = _open_planning_db(planning_db)

    try:
        if reuse_index:
            node_count, relationship_count = _planning_db_counts(db)
        else:
            print(
                f"[gpu-storage] building bounded-memory graph/index database for {graph}",
                file=sys.stderr,
                flush=True,
            )
            node_count, relationship_count = _index_graph(graph, db)

        # cache_mask is planner scratch space. Base GPU distribution and cache
        # planning are intentionally serialized and never persist state here.
        db.execute("UPDATE nodes SET cache_mask=0")
        db.commit()

        gpu_owned = int(db.execute("SELECT count(*) FROM nodes WHERE source='GPU'").fetchone()[0])
        if gpu_owned and not gpus:
            raise ValueError(
                "Graph contains authoritative GPU vertices but GPU distribution enables no devices"
            )

        all_gpu_bits = _all_gpu_bits(gpus)
        remaining: dict[int, dict[str, int]] = {}
        region_results: list[dict[str, Any]] = []

        for gpu in gpus:
            device_id = int(gpu["device_id"])
            memory_limit = _bytes(gpu.get("memory_limit_bytes", gpu.get("memory", 0)))
            memory_utilization = float(gpu.get("memory_utilization", 0.80))
            if not 0.0 < memory_utilization <= 1.0:
                raise ValueError("gpu.memory_utilization must be in the interval (0, 1]")
            usable_memory = int(memory_limit * memory_utilization) if memory_limit else 0
            relation_limit = int(gpu.get("relation_limit", gpu.get("relations", 0)) or 0)
            planning_bytes_per_relation = int(gpu.get("planning_bytes_per_relation", 40) or 40)
            vertex_overhead_bytes = int(gpu.get("vertex_overhead_bytes", 32) or 32)
            remaining[device_id] = {
                "memory": usable_memory,
                "relations": relation_limit,
            }

            for region_index, region in enumerate(gpu.get("regions", [])):
                policy = dict(region.get("policy", {}))
                if not policy:
                    policy = {"kind": region.get("kind", "top-degree")}
                if region.get("node_types") or region.get("nodeTypes"):
                    policy.setdefault(
                        "node_types", region.get("node_types", region.get("nodeTypes"))
                    )
                stats = _select_policy(
                    db,
                    policy=policy,
                    location_bit=gpu_location_bit(device_id),
                    memory_budget=max(0, remaining[device_id]["memory"]),
                    relation_budget=max(0, remaining[device_id]["relations"]),
                    vertex_budget=int(
                        region.get("vertex_limit", region.get("vertices", 0)) or 0
                    ),
                    allowed_sources=("GPU",),
                    exclude_mask=all_gpu_bits,
                    fixed_overhead_bytes=vertex_overhead_bytes,
                    neighbor_bytes=planning_bytes_per_relation,
                )
                if remaining[device_id]["memory"]:
                    remaining[device_id]["memory"] = max(
                        0, remaining[device_id]["memory"] - int(stats["estimatedBytes"])
                    )
                if remaining[device_id]["relations"]:
                    remaining[device_id]["relations"] = max(
                        0,
                        remaining[device_id]["relations"]
                        - int(stats["estimatedRelations"]),
                    )
                region_results.append(
                    {
                        "location": f"GPU:{device_id}",
                        "deviceId": device_id,
                        "region": str(region.get("name", f"region-{region_index}")),
                        "priority": int(region.get("priority", gpu.get("priority", 0))),
                        "planningBytesPerRelation": planning_bytes_per_relation,
                        "memoryUtilization": memory_utilization,
                        **stats,
                    }
                )

        if gpu_owned:
            unassigned = int(
                db.execute(
                    "SELECT count(*) FROM nodes WHERE source='GPU' AND (cache_mask & ?) = 0",
                    (all_gpu_bits,),
                ).fetchone()[0]
            )
            if unassigned:
                fallback_device = int(
                    config.get("fallback_device_id", int(gpus[0]["device_id"]))
                )
                fallback_gpu = next(
                    (gpu for gpu in gpus if int(gpu["device_id"]) == fallback_device),
                    None,
                )
                if fallback_gpu is None:
                    raise ValueError(
                        f"fallback_device_id={fallback_device} is not an enabled GPU"
                    )
                stats = _select_policy(
                    db,
                    policy={"kind": "all"},
                    location_bit=gpu_location_bit(fallback_device),
                    memory_budget=max(0, remaining[fallback_device]["memory"]),
                    relation_budget=max(0, remaining[fallback_device]["relations"]),
                    allowed_sources=("GPU",),
                    exclude_mask=all_gpu_bits,
                    fixed_overhead_bytes=int(
                        fallback_gpu.get("vertex_overhead_bytes", 32) or 32
                    ),
                    neighbor_bytes=int(
                        fallback_gpu.get("planning_bytes_per_relation", 40) or 40
                    ),
                )
                region_results.append(
                    {
                        "location": f"GPU:{fallback_device}",
                        "deviceId": fallback_device,
                        "region": "fallback",
                        "priority": int(fallback_gpu.get("priority", 0)),
                        **stats,
                    }
                )

            unassigned = int(
                db.execute(
                    "SELECT count(*) FROM nodes WHERE source='GPU' AND (cache_mask & ?) = 0",
                    (all_gpu_bits,),
                ).fetchone()[0]
            )
            if unassigned:
                raise ValueError(
                    f"{unassigned:,} authoritative GPU vertices do not fit in the configured GPU distribution"
                )

        gpu_artifacts: dict[str, Any] = {}
        for gpu in gpus:
            device_id = int(gpu["device_id"])
            resident = int(
                db.execute(
                    "SELECT count(*) FROM nodes WHERE source='GPU' AND (cache_mask & ?) != 0",
                    (gpu_location_bit(device_id),),
                ).fetchone()[0]
            )
            if resident == 0:
                continue
            artifact = _build_gpu_projection_artifact(
                db,
                output / "gpu" / f"gpu{device_id}",
                device_id,
                gpu_location_bit(device_id),
            )
            manifest = json.loads(artifact.manifest_path.read_text(encoding="utf-8"))
            gpu_artifacts[str(device_id)] = {
                "manifest": str(Path("gpu") / f"gpu{device_id}" / "manifest.json"),
                "residentVertices": artifact.resident_vertices,
                "vertices": artifact.vertices,
                "adjacencyPairs": artifact.adjacency_pairs,
                "bytes": artifact.bytes,
                "estimatedPeakDeviceBytes": int(
                    manifest.get("estimatedPeakDeviceBytes", 0)
                ),
            }

        ownership = (
            _write_ownership(db, output, all_gpu_bits)
            if gpu_owned
            else {
                "path": "ownership.bin",
                "entries": 0,
                "bytes": 0,
                "record": "<uint64 canonical_id,uint32 device_id,uint32 reserved>",
                "devices": [],
            }
        )
        if not gpu_owned:
            (output / "ownership.bin").write_bytes(b"")

        plan = {
            "format": "polygraph.gpu-storage-plan",
            "version": 1,
            "graph": str(graph),
            "nodes": node_count,
            "relationships": relationship_count,
            "gpuOwnedVertices": gpu_owned,
            "graphRouting": {
                "idRouting": list(metadata.get("idRouting", [])),
                "idRoutingBits": metadata.get("idRoutingBits"),
            },
            "locationEncoding": "external-gpu-ownership-v1",
            "regions": region_results,
            "gpus": gpus,
            "fallbackDeviceId": (
                int(config.get("fallback_device_id", int(gpus[0]["device_id"])))
                if gpus
                else None
            ),
            "artifacts": {"ownership": ownership, "gpu": gpu_artifacts},
            "planningDatabase": str(planning_db),
            "reusedPlanningDatabase": bool(reuse_index),
            "runtimeBackends": _runtime_gpu_backends(config, output, gpu_artifacts),
        }
        plan_path = output / "gpu-storage-plan.json"
        plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
        return {**plan, "plan": str(plan_path)}
    finally:
        try:
            db.execute("UPDATE nodes SET cache_mask=0")
            db.commit()
        finally:
            db.close()
