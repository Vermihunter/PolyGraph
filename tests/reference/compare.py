"""Strict CSV and semantic checks for the independent, test-only BFS oracle.

This module deliberately uses only the standard library and the oracle's plain
data model. In particular, it does not share the production CSV comparison code.
"""

from collections.abc import Mapping
import csv
from dataclasses import dataclass
from pathlib import Path
import re

from .model import BfsResult, Projection, Vertex


_NODE_TYPES = frozenset(("Comment", "Forum", "Organisation", "Person", "Place", "Post", "Tag", "TagClass"))
_MAX_ID = (1 << 64) - 1
_DECIMAL = re.compile(r"(?:0|[1-9][0-9]*)\Z")
_BASIC_HEADER = ("vertex_type", "vertex_id", "distance")
_PARENT_HEADER = _BASIC_HEADER + ("predecessor_type", "predecessor_id")


@dataclass(frozen=True)
class Comparison:
    missing: tuple[Vertex, ...]
    extra: tuple[Vertex, ...]
    wrong_distance: tuple[tuple[Vertex, int, int], ...]
    invalid_predecessor: tuple[tuple[Vertex, str], ...]

    @property
    def same(self) -> bool:
        return not (self.missing or self.extra or self.wrong_distance or self.invalid_predecessor)


def _validate_vertex(vertex: Vertex, context: str) -> None:
    if (not isinstance(vertex, tuple) or len(vertex) != 2
            or not isinstance(vertex[0], str) or vertex[0] not in _NODE_TYPES
            or type(vertex[1]) is not int or not 0 <= vertex[1] <= _MAX_ID):
        raise ValueError(f"{context}: invalid typed uint64 vertex {vertex!r}")


def _validate_distance(distance: int, context: str) -> None:
    if type(distance) is not int or distance < 0:
        raise ValueError(f"{context}: distance must be a nonnegative integer")


def _validate_result(result: BfsResult, context: str) -> None:
    if not isinstance(result.distance, Mapping) or not isinstance(result.predecessor, Mapping):
        raise ValueError(f"{context}: distance and predecessor must be mappings")
    for vertex, distance in result.distance.items():
        _validate_vertex(vertex, context)
        _validate_distance(distance, context)
    for vertex, parent in result.predecessor.items():
        _validate_vertex(vertex, context)
        _validate_vertex(parent, context)


def _decimal(value: str, context: str, *, identifier: bool = False) -> int:
    if not _DECIMAL.fullmatch(value):
        raise ValueError(f"{context}: expected canonical unsigned decimal, got {value!r}")
    number = int(value)
    if identifier and number > _MAX_ID:
        raise ValueError(f"{context}: vertex ID exceeds uint64")
    return number


def _csv_vertex(vertex_type: str, vertex_id: str, context: str) -> Vertex:
    vertex = (vertex_type, _decimal(vertex_id, context, identifier=True))
    _validate_vertex(vertex, context)
    return vertex


def read_result(path: Path, *, require_predecessors: bool = False) -> BfsResult:
    """Read an exact-schema result CSV, rejecting ambiguous or repeated rows."""
    distances: dict[Vertex, int] = {}
    predecessors: dict[Vertex, Vertex] = {}
    try:
        with path.open("r", encoding="utf-8", newline="") as stream:
            reader = csv.reader(stream, strict=True)
            header = tuple(next(reader, ()))
            if header not in (_BASIC_HEADER, _PARENT_HEADER):
                raise ValueError(f"{path}: invalid result CSV header")
            extended = header == _PARENT_HEADER
            if require_predecessors and not extended:
                raise ValueError(f"{path}: predecessor columns are required")
            for row in reader:
                context = f"{path}: row ending at line {reader.line_num}"
                if len(row) != len(header):
                    raise ValueError(f"{context}: expected {len(header)} columns, got {len(row)}")
                vertex = _csv_vertex(row[0], row[1], context)
                if vertex in distances:
                    raise ValueError(f"{context}: duplicate vertex {vertex!r}")
                distance = _decimal(row[2], context)
                if extended:
                    parent_type, parent_id = row[3:]
                    if bool(parent_type) != bool(parent_id):
                        raise ValueError(f"{context}: predecessor pair must be entirely present or empty")
                    if parent_type:
                        predecessors[vertex] = _csv_vertex(parent_type, parent_id, context)
                distances[vertex] = distance
    except (csv.Error, UnicodeError) as error:
        raise ValueError(f"{path}: malformed result CSV: {error}") from error
    return BfsResult(distance=distances, predecessor=predecessors)


def write_result(result: BfsResult, path: Path, *, include_predecessors: bool = True) -> None:
    """Write canonical decimal values sorted by distance, type, and numeric ID."""
    _validate_result(result, "result to write")
    if result.predecessor.keys() - result.distance.keys():
        raise ValueError("result to write: predecessor entry belongs to an unreached vertex")
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(_PARENT_HEADER if include_predecessors else _BASIC_HEADER)
        for vertex in sorted(result.distance, key=lambda item: (result.distance[item], item[0], item[1])):
            row = [vertex[0], vertex[1], result.distance[vertex]]
            if include_predecessors:
                parent = result.predecessor.get(vertex)
                row.extend(parent if parent is not None else ("", ""))
            writer.writerow(row)


def _validate_projection(projection: Projection, source: Vertex) -> None:
    _validate_vertex(source, "source")
    for vertex in projection.vertices:
        _validate_vertex(vertex, "graph vertex")
    if source not in projection.vertices:
        raise ValueError("source is outside the graph universe")
    if not isinstance(projection.adjacency, Mapping):
        raise ValueError("graph adjacency must be a mapping")
    for vertex, neighbors in projection.adjacency.items():
        _validate_vertex(vertex, "graph adjacency key")
        if vertex not in projection.vertices:
            raise ValueError("graph adjacency key is outside the graph universe")
        if not isinstance(neighbors, (tuple, list, set, frozenset)):
            raise ValueError("graph neighbors must be a finite collection of vertices")
        for neighbor in neighbors:
            _validate_vertex(neighbor, "graph adjacency neighbor")
            if neighbor not in projection.vertices:
                raise ValueError("graph adjacency neighbor is outside the graph universe")


def compare_results(
    expected: BfsResult,
    actual: BfsResult,
    projection: Projection,
    source: Vertex,
    *,
    max_depth: int | None = None,
    require_predecessors: bool = True,
) -> Comparison:
    """Compare exact distance maps and validate an actual shortest-parent tree.

    Expected parents are not compared: any projected parent edge whose reached
    parent has distance one less than its child is an acceptable witness.
    Malformed programmatic values raise ValueError. Semantic defects in actual
    results are returned as diagnostics, including graph/source/depth defects.
    """
    _validate_result(expected, "expected result")
    _validate_result(actual, "actual result")
    _validate_projection(projection, source)
    if max_depth is not None:
        _validate_distance(max_depth, "maximum depth")
    if expected.distance.get(source) != 0:
        raise ValueError("expected source distance must be zero")
    for vertex, distance in expected.distance.items():
        if vertex not in projection.vertices:
            raise ValueError("expected result contains a vertex outside the graph universe")
        if max_depth is not None and distance > max_depth:
            raise ValueError("expected result exceeds maximum depth")
    for vertex, parent in expected.predecessor.items():
        if vertex not in expected.distance or parent not in projection.vertices:
            raise ValueError("expected predecessor entry lies outside its result or graph universe")

    expected_vertices = expected.distance.keys()
    actual_vertices = actual.distance.keys()
    missing = tuple(sorted(expected_vertices - actual_vertices))
    extra = tuple(sorted(actual_vertices - expected_vertices))
    wrong_distance = tuple(
        (vertex, expected.distance[vertex], actual.distance[vertex])
        for vertex in sorted(expected_vertices & actual_vertices)
        if expected.distance[vertex] != actual.distance[vertex]
    )
    invalid: list[tuple[Vertex, str]] = []
    if actual.distance.get(source) != 0:
        invalid.append((source, "source must be reached with distance zero"))
    for vertex, distance in actual.distance.items():
        if vertex not in projection.vertices:
            invalid.append((vertex, "vertex is outside the graph universe"))
        if max_depth is not None and distance > max_depth:
            invalid.append((vertex, "distance exceeds maximum depth"))
        if vertex != source and distance == 0:
            invalid.append((vertex, "only the source may have distance zero"))

    if not require_predecessors:
        invalid.extend((vertex, "predecessors are disabled for distance-only comparison")
                       for vertex in actual.predecessor)
    else:
        for vertex in actual_vertices - {source}:
            if vertex not in actual.predecessor:
                invalid.append((vertex, "missing predecessor for reached non-source vertex"))
        neighbors_by_parent: dict[Vertex, frozenset[Vertex]] = {}
        for vertex, parent in actual.predecessor.items():
            if vertex not in actual.distance:
                invalid.append((vertex, "predecessor entry belongs to an unreached vertex"))
            if vertex not in projection.vertices:
                invalid.append((vertex, "predecessor child is outside the graph universe"))
            if vertex == source:
                invalid.append((vertex, "source must not have a predecessor"))
            if parent not in projection.vertices:
                invalid.append((vertex, "parent is outside the graph universe"))
            if parent not in actual.distance:
                invalid.append((vertex, "parent is not reached"))
            neighbors = neighbors_by_parent.get(parent)
            if neighbors is None:
                neighbors = frozenset(projection.adjacency.get(parent, ()))
                neighbors_by_parent[parent] = neighbors
            if vertex not in neighbors:
                invalid.append((vertex, "parent-to-child edge is absent from the projection"))
            if (vertex in actual.distance and parent in actual.distance
                    and actual.distance[parent] + 1 != actual.distance[vertex]):
                invalid.append((vertex, "parent distance plus one does not equal child distance"))

    return Comparison(missing, extra, wrong_distance, tuple(sorted(invalid)))
