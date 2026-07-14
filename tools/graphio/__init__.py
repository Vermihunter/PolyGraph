"""Shared graph input/output helpers used by generators and benchmarks."""

from .id_codec import (
    BACKEND_IDS,
    DEFAULT_ROUTING_BACKENDS,
    BackendLayout,
    backend_id,
    deterministic_entity_uuid,
    deterministic_routed_entity_uuid,
    encode_backend_id,
)
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
    "load_graph_records",
    "open_graph_reader",
    "write_graph_json",
]
