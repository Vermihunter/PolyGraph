"""Plain test-only graph values keyed by LDBC type and raw numeric ID.

This model deliberately has no routed, encoded, database, or production IDs.
"""

from __future__ import annotations

from dataclasses import dataclass

from tests.reference.schema import NODE_TYPES


Vertex = tuple[str, int]
VERTEX_TYPES = frozenset(NODE_TYPES)
UINT64_MAX = (1 << 64) - 1


def validate_vertex(vertex: Vertex) -> None:
    """Reject malformed IDs instead of silently coercing their type or value."""
    if not isinstance(vertex, tuple) or len(vertex) != 2:
        raise ValueError(f"expected a (type, raw numeric ID) tuple, got {vertex!r}")
    node_type, raw_id = vertex
    if not isinstance(node_type, str) or node_type not in VERTEX_TYPES:
        raise ValueError(f"unknown vertex type: {node_type!r}")
    if type(raw_id) is not int or not 0 <= raw_id <= UINT64_MAX:
        raise ValueError(f"raw vertex ID must be an unsigned 64-bit integer: {raw_id!r}")


@dataclass(frozen=True, slots=True)
class Edge:
    source: Vertex
    target: Vertex
    relation: str

    def __post_init__(self) -> None:
        validate_vertex(self.source)
        validate_vertex(self.target)
        if not isinstance(self.relation, str) or not self.relation:
            raise ValueError("edge relation must be a nonempty string")


@dataclass(frozen=True, slots=True)
class Graph:
    vertices: frozenset[Vertex]
    edges: tuple[Edge, ...]

    def __post_init__(self) -> None:
        for vertex in self.vertices:
            validate_vertex(vertex)
        for edge in self.edges:
            if not isinstance(edge, Edge):
                raise ValueError(f"expected an Edge, got {edge!r}")
            if edge.source not in self.vertices or edge.target not in self.vertices:
                raise ValueError(f"edge has an endpoint absent from vertices: {edge!r}")


@dataclass(frozen=True, slots=True)
class Projection:
    vertices: frozenset[Vertex]
    adjacency: dict[Vertex, tuple[Vertex, ...]]


@dataclass(frozen=True, slots=True)
class BfsResult:
    distance: dict[Vertex, int]
    predecessor: dict[Vertex, Vertex]
