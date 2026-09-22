"""Shared graph input/output helpers used by generators and benchmarks."""

from .id_codec import (
    BACKEND_IDS,
    DEFAULT_ROUTING_BACKENDS,
    BackendLayout,
    backend_id,
    deterministic_entity_uuid,
    deterministic_routed_entity_uuid,
    encode_backend_id,
    canonical_global_id,
    with_cache_residency,
    gpu_location_bit,
    LOCATION_REDIS,
    LOCATION_MMAP,
)
from .cache_planner import load_cache_config, plan_graph_caches
from .gpu_storage_planner import load_gpu_distribution_config, plan_gpu_storage
from .model import GraphRecords, NodeRecord, RelationshipRecord
from .reader import GraphReader, load_graph_records, open_graph_reader
from .writer import GraphJsonLinesWriter, write_graph_json

__all__ = [
    "BACKEND_IDS",
    "BackendLayout",
    "DEFAULT_ROUTING_BACKENDS",
    "GraphJsonLinesWriter",
    "GraphReader",
    "GraphRecords",
    "NodeRecord",
    "RelationshipRecord",
    "backend_id",
    "deterministic_entity_uuid",
    "deterministic_routed_entity_uuid",
    "encode_backend_id",
    "canonical_global_id",
    "with_cache_residency",
    "gpu_location_bit",
    "LOCATION_REDIS",
    "LOCATION_MMAP",
    "load_cache_config",
    "plan_graph_caches",
    "load_gpu_distribution_config",
    "plan_gpu_storage",
    "load_graph_records",
    "open_graph_reader",
    "write_graph_json",
]
