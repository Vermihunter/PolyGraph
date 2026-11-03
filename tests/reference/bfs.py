"""Independent, standard-library projection and FIFO BFS for test fixtures."""

from __future__ import annotations

from collections import deque
from collections.abc import Iterable

from tests.reference.model import BfsResult, Graph, Projection, VERTEX_TYPES, Vertex, validate_vertex
from tests.reference.schema import EDGE_FILES


def _filter_set(values: Iterable[str], allowed: frozenset[str], name: str) -> frozenset[str]:
    if isinstance(values, (str, bytes)):
        raise ValueError(f"{name} must be a collection of names, not one string")
    try:
        names = tuple(values)
    except TypeError as error:
        raise ValueError(f"{name} must be a collection of names") from error
    for value in names:
        if not isinstance(value, str) or value not in allowed:
            raise ValueError(f"unknown {name} name: {value!r}")
    return frozenset(names)


def project(
    graph: Graph,
    *,
    direction: str = "both",
    edge_types: Iterable[str] = (),
    target_types: Iterable[str] = (),
) -> Projection:
    """Filter edge rows, orient transitions, then union their permitted targets.

    Target types restrict every transition's destination. All original vertices
    remain available as roots, including roots outside those target types.
    """
    if direction not in ("outgoing", "both"):
        raise ValueError("direction must be 'outgoing' or 'both'")
    relations = _filter_set(edge_types, frozenset(value[1] for value in EDGE_FILES.values()), "edge_types")
    targets = _filter_set(target_types, VERTEX_TYPES, "target_types")
    neighbors: dict[Vertex, set[Vertex]] = {vertex: set() for vertex in sorted(graph.vertices)}
    for edge in graph.edges:
        if relations and edge.relation not in relations:
            continue
        if not targets or edge.target[0] in targets:
            neighbors[edge.source].add(edge.target)
        if direction == "both" and (not targets or edge.source[0] in targets):
            neighbors[edge.target].add(edge.source)
    return Projection(
        vertices=graph.vertices,
        adjacency={vertex: tuple(sorted(targets)) for vertex, targets in neighbors.items()},
    )


def bfs(
    projection: Projection,
    source: Vertex,
    *,
    max_depth: int | None = None,
    return_predecessors: bool = True,
) -> BfsResult:
    """Find minimum hop counts, discovering each vertex exactly once on enqueue."""
    validate_vertex(source)
    if source not in projection.vertices:
        raise ValueError(f"source is absent from graph vertices: {source!r}")
    if max_depth is not None and (type(max_depth) is not int or max_depth < 0):
        raise ValueError("max_depth must be a nonnegative integer or None")
    distance = {source: 0}
    predecessor: dict[Vertex, Vertex] = {}
    pending = deque((source,))
    while pending:
        current = pending.popleft()
        depth = distance[current]
        if max_depth is not None and depth >= max_depth:
            continue
        for neighbor in sorted(projection.adjacency.get(current, ())):
            if neighbor in distance:
                continue
            distance[neighbor] = depth + 1
            if return_predecessors:
                predecessor[neighbor] = current
            pending.append(neighbor)
    return BfsResult(distance=distance, predecessor=predecessor)
