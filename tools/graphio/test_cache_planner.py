from __future__ import annotations

import json
import struct
from pathlib import Path

from tools.graphio.cache_planner import plan_graph_caches
from tools.graphio.gpu_storage_planner import GPU_OWNER_RECORD, plan_gpu_storage
from tools.graphio.domain import write_backend_config_for_sources
from tools.graphio.id_codec import (
    LOCATION_REDIS,
    BackendLayout,
    canonical_global_id,
    gpu_location_bit,
)
from tools.graphio.model import NodeRecord, RelationshipRecord
from tools.graphio.reader import open_graph_reader
from tools.graphio.writer import GraphJsonLinesWriter

RESIDENCY_RECORD = struct.Struct("<QQ")


def _graph(path: Path) -> BackendLayout:
    layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
    nodes = [
        NodeRecord(layout.encode_local(1, "Neo4J"), "Person", "Neo4J", {}),
        NodeRecord(layout.encode_local(2, "Neo4J"), "Person", "Neo4J", {}),
        NodeRecord(layout.encode_local(3, "ArangoDB"), "Post", "ArangoDB", {}),
        NodeRecord(layout.encode_local(4, "GPU"), "Tag", "GPU", {}),
    ]
    edges = [
        RelationshipRecord(layout.encode_local(10, "Neo4J"), "KNOWS", "Neo4J", nodes[0].uuid, nodes[1].uuid, {}),
        RelationshipRecord(layout.encode_local(11, "ArangoDB"), "LIKES", "ArangoDB", nodes[0].uuid, nodes[2].uuid, {}),
        RelationshipRecord(layout.encode_local(12, "GPU"), "TAGGED", "GPU", nodes[0].uuid, nodes[3].uuid, {}),
    ]
    with GraphJsonLinesWriter(path, metadata=layout.metadata()) as writer:
        for node in nodes:
            writer.write_node(node)
        for edge in edges:
            writer.write_relationship(edge)
    return layout


def _residency(plan_path: Path) -> dict[int, int]:
    plan = json.loads(plan_path.read_text())
    path = plan_path.parent / plan["artifacts"]["residency"]["path"]
    result: dict[int, int] = {}
    with path.open("rb") as handle:
        while record := handle.read(RESIDENCY_RECORD.size):
            assert len(record) == RESIDENCY_RECORD.size
            node_id, mask = RESIDENCY_RECORD.unpack(record)
            result[node_id] = mask
    return result


def test_cache_planner_keeps_graph_immutable_and_builds_portable_artifacts(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    original_graph = graph.read_bytes()
    config = {
        "version": 1,
        "priorities": {"GPU": 0, "MMap": 10, "Redis": 20, "Neo4J": 50, "ArangoDB": 60},
        "gpus": [
            {
                "device_id": 0,
                "memory": "64KiB",
                "relation_limit": 100,
                "regions": [
                    {"name": "hot", "policy": {"kind": "top-degree", "vertex_count": 1}},
                ],
            },
            {
                "device_id": 1,
                "memory": "64KiB",
                "regions": [{"name": "hot-2", "policy": {"kind": "top-degree", "vertex_count": 1}}],
            },
        ],
        "mmap": {"enabled": True, "memory": "64KiB", "vertex_limit": 2, "policy": {"kind": "best"}},
        "redis": {
            "enabled": True,
            "memory": "64KiB",
            "vertex_limit": 2,
            "shared_hot_fraction": 0.075,
            "policy": {"kind": "top-degree"},
        },
    }

    result = plan_graph_caches(graph, config, output_dir=tmp_path / "cache", routing_layout=layout)
    plan_path = Path(result["plan"])
    plan = json.loads(plan_path.read_text())
    assert graph.read_bytes() == original_graph
    assert plan["locationEncoding"] == "external-residency-v1"
    assert plan["artifacts"]["mmap"]["manifest"] == "mmap/manifest.json"
    assert plan["artifacts"]["redis"]["manifest"] == "redis/manifest.json"
    assert plan["artifacts"]["gpu"]["0"]["manifest"] == "gpu/gpu0/manifest.json"
    assert plan["artifacts"]["residency"]["path"] == "residency.bin"

    nodes = list(open_graph_reader(graph).iter_nodes())
    edges = list(open_graph_reader(graph).iter_relationships())
    hot = max(nodes, key=lambda node: sum(1 for edge in edges if edge.from_uuid == node.uuid or edge.to_uuid == node.uuid))
    mask = _residency(plan_path)[canonical_global_id(hot.uuid)]
    assert mask & gpu_location_bit(0)
    assert mask & gpu_location_bit(1)

    assert (tmp_path / "cache/mmap/index.bin").stat().st_size == plan["artifacts"]["mmap"]["vertices"] * 24
    assert (tmp_path / "cache/redis/neighbors.bin").stat().st_size == plan["artifacts"]["redis"]["relations"] * 8
    assert (tmp_path / "cache-plan.sqlite3").is_file()


def test_backend_config_keeps_gpu_storage_separate_from_cache_backends(tmp_path: Path) -> None:
    distribution = {
        "priorities": {"GPU": 2},
        "gpus": [
            {"device_id": 0, "memory": "2GiB", "relation_limit": 123},
            {"device_id": 3, "memory": "1GiB", "relation_limit": 77, "priority": 1},
        ],
    }
    plan_path = tmp_path / "gpu-storage" / "gpu-storage-plan.json"
    plan_path.parent.mkdir()
    plan_path.write_text("{}\n")

    path = write_backend_config_for_sources(
        ("Neo4J", "ArangoDB", "GPU"),
        tmp_path / "backends.json",
        arango_database="test",
        cache_config={
            "gpus": [{"device_id": 7}],
            "mmap": {"enabled": True},
            "redis": {"enabled": True},
        },
        gpu_distribution_config=distribution,
        gpu_storage_plan=plan_path,
    )
    raw = json.loads(path.read_text())
    entries = {entry["id"]: entry for entry in raw["backends"]}
    assert "GPU:0" not in entries
    assert "GPU:3" not in entries
    assert "GPU:7" not in entries
    assert "Redis" not in entries
    assert "MMap" not in entries
    assert raw["gpuStoragePlan"] == "gpu-storage/gpu-storage-plan.json"


def test_gpu_storage_distribution_and_cache_selection_are_disjoint(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
    nodes = [
        NodeRecord(layout.encode_local(1, "Neo4J"), "Person", "Neo4J", {}),
        NodeRecord(layout.encode_local(2, "ArangoDB"), "Post", "ArangoDB", {}),
        NodeRecord(layout.encode_local(3, "GPU"), "Tag", "GPU", {}),
        NodeRecord(layout.encode_local(4, "GPU"), "Tag", "GPU", {}),
        NodeRecord(layout.encode_local(5, "GPU"), "Tag", "GPU", {}),
        NodeRecord(layout.encode_local(6, "GPU"), "Tag", "GPU", {}),
    ]
    edges = [
        RelationshipRecord(
            layout.encode_local(100 + index, "Neo4J"),
            "LINK",
            "Neo4J",
            nodes[0].uuid,
            node.uuid,
            {},
        )
        for index, node in enumerate(nodes[1:])
    ]
    with GraphJsonLinesWriter(graph, metadata=layout.metadata()) as writer:
        for node in nodes:
            writer.write_node(node)
        for edge in edges:
            writer.write_relationship(edge)

    storage = plan_gpu_storage(
        graph,
        {
            "version": 1,
            "fallback_device_id": 1,
            "gpus": [
                {
                    "device_id": 0,
                    "regions": [
                        {
                            "name": "first-half",
                            "policy": {"kind": "top-degree", "vertex_fraction": 0.5},
                        }
                    ],
                },
                {
                    "device_id": 1,
                    "regions": [{"name": "remaining", "policy": {"kind": "all"}}],
                },
            ],
        },
        output_dir=tmp_path / "gpu-storage",
        planning_db_path=tmp_path / "cache-plan.sqlite3",
        routing_layout=layout,
    )
    storage_plan = json.loads(Path(storage["plan"]).read_text())
    ownership_path = Path(storage["plan"]).parent / storage_plan["artifacts"]["ownership"]["path"]
    ownership = list(struct.iter_unpack(GPU_OWNER_RECORD.format, ownership_path.read_bytes()))
    assert len(ownership) == 4
    assert {device for _canonical, device, _reserved in ownership} == {0, 1}
    assert len({canonical for canonical, _device, _reserved in ownership}) == 4
    assert set(storage_plan["artifacts"]["gpu"]) == {"0", "1"}

    cache = plan_graph_caches(
        graph,
        {
            "version": 1,
            "gpus": [
                {
                    "device_id": 0,
                    "regions": [{"name": "all-cacheable", "policy": {"kind": "all"}}],
                }
            ],
            "mmap": {"enabled": True, "policy": {"kind": "all"}},
            "redis": {
                "enabled": True,
                "shared_hot_fraction": 0.0,
                "policy": {"kind": "all"},
            },
        },
        output_dir=tmp_path / "cache",
        planning_db_path=tmp_path / "cache-plan.sqlite3",
        routing_layout=layout,
    )
    cached_ids = set(_residency(Path(cache["plan"])))
    gpu_owned_ids = {
        canonical_global_id(node.uuid)
        for node in nodes
        if node.source == "GPU"
    }
    assert cached_ids.isdisjoint(gpu_owned_ids)


def test_cache_rejects_legacy_authoritative_gpu_region(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    import pytest

    with pytest.raises(ValueError, match="authoritative GPU-owned"):
        plan_graph_caches(
            graph,
            {
                "version": 1,
                "gpus": [
                    {
                        "device_id": 0,
                        "regions": [
                            {
                                "name": "legacy-primary",
                                "policy": {
                                    "kind": "mapped",
                                    "owner_sources": ["GPU"],
                                },
                            }
                        ],
                    }
                ],
                "mmap": {"enabled": False},
                "redis": {"enabled": False},
            },
            output_dir=tmp_path / "cache",
            routing_layout=layout,
        )


def test_cache_planner_reconfigures_residency_without_rewriting_graph(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    original_graph = graph.read_bytes()
    first = {
        "version": 1,
        "gpus": [],
        "mmap": {"enabled": False},
        "redis": {"enabled": True, "memory": "64KiB", "vertex_limit": 2, "policy": {"kind": "top-degree"}},
    }
    first_result = plan_graph_caches(graph, first, output_dir=tmp_path / "cache-a", routing_layout=layout)
    first_residency = _residency(Path(first_result["plan"]))
    assert any(mask & LOCATION_REDIS for mask in first_residency.values())
    assert graph.read_bytes() == original_graph

    second = {
        "version": 1,
        "gpus": [{
            "device_id": 0,
            "memory": "64KiB",
            "regions": [{"name": "hot", "policy": {"kind": "top-degree", "vertex_count": 1}}],
        }],
        "mmap": {"enabled": False},
        "redis": {"enabled": False},
    }
    second_result = plan_graph_caches(graph, second, output_dir=tmp_path / "cache-b", routing_layout=layout)
    second_residency = _residency(Path(second_result["plan"]))
    assert second_result["reusedPlanningDatabase"] is True
    assert not any(mask & LOCATION_REDIS for mask in second_residency.values())
    assert any(mask & gpu_location_bit(0) for mask in second_residency.values())
    assert graph.read_bytes() == original_graph


def test_cache_planner_reuses_persistent_sqlite_without_reindex(tmp_path: Path, monkeypatch) -> None:
    import tools.graphio.cache_planner as planner

    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    config = {
        "version": 1,
        "gpus": [],
        "mmap": {"enabled": False},
        "redis": {"enabled": True, "memory": "64KiB", "vertex_limit": 2, "policy": {"kind": "top-degree"}},
    }

    first = plan_graph_caches(graph, config, output_dir=tmp_path / "cache-a", routing_layout=layout)
    planning_db = tmp_path / "cache-plan.sqlite3"
    assert planning_db.is_file()
    assert first["reusedPlanningDatabase"] is False

    def must_not_reindex(*args, **kwargs):
        raise AssertionError("reusable planning database unexpectedly reindexed graph.jsonl")

    monkeypatch.setattr(planner, "_index_graph", must_not_reindex)
    second = plan_graph_caches(graph, config, output_dir=tmp_path / "cache-b", routing_layout=layout)
    assert second["reusedPlanningDatabase"] is True
    assert Path(second["plan"]).is_file()
    assert (tmp_path / "cache-b/residency.bin").is_file()


def test_redis_uses_small_shared_hot_tier_then_unique_vertices(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    config = {
        "version": 1,
        "gpus": [{
            "device_id": 0,
            "memory": "64KiB",
            "regions": [{"name": "hot", "policy": {"kind": "top-degree", "vertex_count": 2}}],
        }],
        "mmap": {"enabled": False},
        "redis": {
            "enabled": True,
            "memory": "64KiB",
            "vertex_limit": 2,
            "shared_hot_fraction": 0.5,
            "policy": {"kind": "top-degree"},
        },
    }

    result = plan_graph_caches(graph, config, output_dir=tmp_path / "cache", routing_layout=layout)
    redis_region = next(region for region in result["regions"] if region["location"] == "Redis")
    assert redis_region["vertices"] == 2
    assert redis_region["sharedHotVertices"] == 1
    assert redis_region["redisUniqueVertices"] == 1

    residency = _residency(Path(result["plan"]))
    gpu = gpu_location_bit(0)
    redis_ids = {node_id for node_id, mask in residency.items() if mask & LOCATION_REDIS}
    gpu_ids = {node_id for node_id, mask in residency.items() if mask & gpu}
    assert len(redis_ids & gpu_ids) == 1
    assert len(redis_ids - gpu_ids) == 1


def test_planning_database_rebuilds_when_graph_revision_changes(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    config = {
        "version": 1,
        "gpus": [],
        "mmap": {"enabled": False},
        "redis": {"enabled": True, "memory": "64KiB", "vertex_limit": 1, "policy": {"kind": "top-degree"}},
    }

    first = plan_graph_caches(graph, config, output_dir=tmp_path / "cache-a", routing_layout=layout)
    assert first["reusedPlanningDatabase"] is False

    # A blank JSONL line is semantically harmless but changes the graph signature.
    with graph.open("ab") as handle:
        handle.write(b"\n")

    second = plan_graph_caches(graph, config, output_dir=tmp_path / "cache-b", routing_layout=layout)
    assert second["reusedPlanningDatabase"] is False
    assert second["nodes"] == first["nodes"]
    assert second["relationships"] == first["relationships"]


def test_gpu_projection_artifact_is_dense_reusable_and_contains_only_resident_source_adjacency(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    config = {
        "version": 1,
        "gpus": [{
            "device_id": 0,
            "memory": "64KiB",
            "regions": [{"name": "hot", "policy": {"kind": "top-degree", "vertex_count": 1}}],
        }],
        "mmap": {"enabled": False},
        "redis": {"enabled": False},
    }

    result = plan_graph_caches(graph, config, output_dir=tmp_path / "cache", routing_layout=layout)
    plan = json.loads(Path(result["plan"]).read_text())
    artifact = plan["artifacts"]["gpu"]["0"]
    manifest_path = tmp_path / "cache" / artifact["manifest"]
    manifest = json.loads(manifest_path.read_text())

    assert manifest["format"] == "polygraph.gpu-projection.dense-edgelist"
    assert manifest["residentVertices"] == 1
    assert manifest["vertices"] >= manifest["residentVertices"]
    assert manifest["adjacencyPairs"] > 0

    ids_raw = (manifest_path.parent / manifest["globalIds"]).read_bytes()
    source_raw = (manifest_path.parent / manifest["sources"]).read_bytes()
    destination_raw = (manifest_path.parent / manifest["destinations"]).read_bytes()
    ids = [value[0] for value in struct.iter_unpack("<Q", ids_raw)]
    sources = [value[0] for value in struct.iter_unpack("<I", source_raw)]
    destinations = [value[0] for value in struct.iter_unpack("<I", destination_raw)]

    assert ids == sorted(set(ids))
    assert len(sources) == len(destinations) == manifest["adjacencyPairs"]
    assert all(source < len(ids) for source in sources)
    assert all(destination < len(ids) for destination in destinations)
    assert list(zip(sources, destinations)) == sorted(set(zip(sources, destinations)))

    residency = _residency(Path(result["plan"]))
    resident_gpu_ids = {
        node_id for node_id, mask in residency.items() if mask & gpu_location_bit(0)
    }
    # Proxy/destination IDs may appear in the dense map, but only actually
    # GPU-resident vertices are materialized as expansion sources.
    assert {ids[source] for source in sources} <= resident_gpu_ids


def test_fractional_rank_windows_create_non_overlapping_benchmark_tiers(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    config = {
        "version": 1,
        "gpus": [{
            "device_id": 0,
            "regions": [{
                "name": "gpu-top-half",
                "policy": {"kind": "top-degree", "vertex_fraction": 0.5},
            }],
        }],
        "mmap": {
            "enabled": True,
            "policy": {
                "kind": "top-degree",
                "vertex_fraction": 0.25,
                "rank_start_fraction": 0.75,
            },
        },
        "redis": {
            "enabled": True,
            "shared_hot_fraction": 0.0,
            "policy": {"kind": "top-degree", "vertex_fraction": 0.25},
        },
    }

    result = plan_graph_caches(graph, config, output_dir=tmp_path / "cache", routing_layout=layout)
    regions = {region["location"]: region for region in result["regions"]}
    assert regions["GPU:0"]["vertices"] == 2
    assert regions["Redis"]["vertices"] == 1
    assert regions["MMap"]["vertices"] == 1
    assert regions["GPU:0"]["requestedVertexFraction"] == 0.5
    assert regions["MMap"]["rankSkippedVertices"] == 2

    from tools.graphio.id_codec import LOCATION_MMAP

    residency = _residency(Path(result["plan"]))
    gpu_ids = {node_id for node_id, mask in residency.items() if mask & gpu_location_bit(0)}
    redis_ids = {node_id for node_id, mask in residency.items() if mask & LOCATION_REDIS}
    mmap_ids = {node_id for node_id, mask in residency.items() if mask & LOCATION_MMAP}
    assert len(gpu_ids) == 2
    assert len(redis_ids) == 1
    assert len(mmap_ids) == 1
    assert not (gpu_ids & redis_ids)
    assert not (gpu_ids & mmap_ids)
    # Only three vertices are cache-eligible after authoritative GPU-owned
    # vertices are excluded, so the Redis and mmap rank windows may overlap.


def test_full_gpu_cache_enables_native_bfs_only_when_every_vertex_is_resident(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = BackendLayout(("Neo4J", "ArangoDB"))
    nodes = [
        NodeRecord(layout.encode_local(1, "Neo4J"), "Person", "Neo4J", {}),
        NodeRecord(layout.encode_local(2, "ArangoDB"), "Person", "ArangoDB", {}),
        NodeRecord(layout.encode_local(3, "Neo4J"), "Person", "Neo4J", {}),
    ]
    edges = [
        RelationshipRecord(layout.encode_local(10, "Neo4J"), "KNOWS", "Neo4J", nodes[0].uuid, nodes[1].uuid, {}),
        RelationshipRecord(layout.encode_local(11, "ArangoDB"), "KNOWS", "ArangoDB", nodes[1].uuid, nodes[2].uuid, {}),
    ]
    with GraphJsonLinesWriter(graph, metadata=layout.metadata()) as writer:
        for node in nodes:
            writer.write_node(node)
        for edge in edges:
            writer.write_relationship(edge)

    full = plan_graph_caches(
        graph,
        {
            "version": 1,
            "gpus": [{"device_id": 0, "regions": [{"name": "all", "policy": {"kind": "all"}}]}],
            "mmap": {"enabled": False},
            "redis": {"enabled": False},
        },
        output_dir=tmp_path / "full",
        planning_db_path=tmp_path / "planning.sqlite3",
        routing_layout=layout,
    )
    full_backend = full["runtimeBackends"][0]
    assert full["artifacts"]["gpu"]["0"]["residentVertices"] == len(nodes)
    assert full_backend["capabilities"]["native_bfs"] is True
    assert full_backend["coverage"]["complete_for_requested_subgraph"] is True

    partial = plan_graph_caches(
        graph,
        {
            "version": 1,
            "gpus": [{
                "device_id": 0,
                "regions": [{"name": "one", "policy": {"kind": "top-degree", "vertex_count": 1}}],
            }],
            "mmap": {"enabled": False},
            "redis": {"enabled": False},
        },
        output_dir=tmp_path / "partial",
        planning_db_path=tmp_path / "planning.sqlite3",
        routing_layout=layout,
    )
    partial_backend = partial["runtimeBackends"][0]
    assert partial["artifacts"]["gpu"]["0"]["residentVertices"] < len(nodes)
    assert partial_backend["capabilities"]["native_bfs"] is False
    assert partial_backend["coverage"]["complete_for_requested_subgraph"] is False


def test_full_mmap_cache_marks_runtime_backend_complete(tmp_path: Path) -> None:
    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    plan = plan_graph_caches(
        graph,
        {
            "version": 1,
            "gpus": [],
            "mmap": {"enabled": True, "include_gpu_owned": True, "policy": {"kind": "all"}},
            "redis": {"enabled": False},
        },
        output_dir=tmp_path / "full-mmap",
        planning_db_path=tmp_path / "planning.sqlite3",
        routing_layout=layout,
    )

    mmap_backend = next(backend for backend in plan["runtimeBackends"] if backend["id"] == "MMap")
    assert plan["artifacts"]["mmap"]["vertices"] == plan["nodes"]
    assert mmap_backend["coverage"]["complete_for_requested_subgraph"] is True


def test_mmap_can_fill_only_vertices_not_already_cached_by_gpu_or_redis(tmp_path: Path) -> None:
    from tools.graphio.id_codec import LOCATION_MMAP

    graph = tmp_path / "graph.jsonl"
    layout = _graph(graph)
    config = {
        "version": 1,
        "gpus": [{
            "device_id": 0,
            "regions": [{"name": "hot", "policy": {"kind": "top-degree", "vertex_count": 1}}],
        }],
        "redis": {
            "enabled": True,
            "shared_hot_fraction": 0.0,
            "vertex_limit": 1,
            "policy": {"kind": "top-degree"},
        },
        "mmap": {
            "enabled": True,
            "policy": {
                "kind": "all",
                "exclude_locations": ["GPU:0", "Redis"],
            },
        },
    }

    result = plan_graph_caches(graph, config, output_dir=tmp_path / "cache", routing_layout=layout)
    residency = _residency(Path(result["plan"]))
    gpu_ids = {node_id for node_id, mask in residency.items() if mask & gpu_location_bit(0)}
    redis_ids = {node_id for node_id, mask in residency.items() if mask & LOCATION_REDIS}
    mmap_ids = {node_id for node_id, mask in residency.items() if mask & LOCATION_MMAP}

    assert len(gpu_ids) == 1
    assert len(redis_ids) == 1
    assert len(mmap_ids) == 1
    assert not (gpu_ids & redis_ids)
    assert not (gpu_ids & mmap_ids)
    assert not (redis_ids & mmap_ids)
