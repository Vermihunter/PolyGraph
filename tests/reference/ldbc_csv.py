"""Strict, stdlib-only reader for initial SNB Interactive v1 CsvBasic data.

The oracle retains the CSV topology and raw typed IDs directly.  It never uses
production parsing, ID conversion, property models, routing, or storage code.
"""

from collections.abc import Iterator
import csv
from pathlib import Path
import re

from .model import Edge, Graph, Vertex
from .schema import AUX_FILES, EDGE_FILES, NODE_FILES


_SHARD_NAME = re.compile(r"(.+)_([0-9]+)_([0-9]+)\.csv\Z")
_DECIMAL_ID = re.compile(r"[0-9]+\Z")
_MAX_ID = (1 << 64) - 1


def _discover(root: Path, require_complete: bool) -> list[tuple[Path, str]]:
    """Check the whole CSV inventory before loading any node or edge rows."""
    for area in ("static", "dynamic"):
        directory = root / area
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError(f"missing initial-data directory or symlink: {directory}")
    families = NODE_FILES | EDGE_FILES | AUX_FILES
    found: set[str] = set()
    files: list[tuple[Path, str]] = []
    for path in sorted(root.rglob("*"), key=lambda value: value.relative_to(root).as_posix()):
        relative = path.relative_to(root)
        if path.is_symlink() and relative.parts[0] in {"static", "dynamic"}:
            raise ValueError(f"symlinks are not initial-data files: {relative}")
        if path.suffix.lower() != ".csv":
            continue
        if len(relative.parts) == 1 and path.name.startswith("updateStream") and path.suffix == ".csv":
            continue
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"not a regular initial CSV file: {relative}")
        match = _SHARD_NAME.fullmatch(path.name)
        if match is None or match[1] not in families:
            raise ValueError(f"unknown CsvBasic CSV family or shard name: {relative}")
        family = match[1]
        area = families[family][0]
        if len(relative.parts) < 2 or relative.parts[0] != area:
            raise ValueError(f"CSV family {family!r} must be below {area}/: {relative}")
        if not path.resolve().is_relative_to(root):
            raise ValueError(f"CSV path escapes initial-data root: {relative}")
        found.add(family)
        files.append((path, family))
    if require_complete:
        missing = sorted(set(families) - found)
        if missing:
            raise ValueError(f"missing CsvBasic file families: {', '.join(missing)}")
    return files


def _rows(path: Path, leading_headers: tuple[str, ...], *, auxiliary: bool = False) -> Iterator[tuple[int, list[str]]]:
    """Validate shape while preserving duplicate endpoint names positionally."""
    try:
        with path.open("r", encoding="utf-8", newline="") as stream:
            reader = csv.reader(stream, delimiter="|", strict=True)
            header = next(reader, None)
            if header and header[-1] == "":
                header.pop()
            if (
                not header
                or any(not column or column.strip() != column for column in header)
                or tuple(header[:len(leading_headers)]) != leading_headers
                or (auxiliary and len(header) != len(leading_headers))
            ):
                raise ValueError(f"{path}: invalid header; expected leading fields {leading_headers!r}")
            # A self-relation legitimately has the same endpoint name twice.
            allowed = list(leading_headers)
            if len(set(header[len(allowed):])) != len(header[len(allowed):]) or any(
                column in allowed for column in header[len(allowed):]
            ):
                raise ValueError(f"{path}: duplicate non-endpoint header")
            width = len(header)
            for row in reader:
                if len(row) == width + 1 and row[-1] == "":
                    row.pop()
                if len(row) != width:
                    raise ValueError(f"{path}:{reader.line_num}: expected {width} columns, got {len(row)}")
                yield reader.line_num, row
    except (UnicodeError, csv.Error) as error:
        raise ValueError(f"{path}: invalid UTF-8 or CSV: {error}") from error


def _raw_id(value: str, path: Path, line: int) -> int:
    if not _DECIMAL_ID.fullmatch(value):
        raise ValueError(f"{path}:{line}: ID is not unsigned ASCII decimal: {value!r}")
    number = int(value)
    if number > _MAX_ID:
        raise ValueError(f"{path}:{line}: ID exceeds uint64: {value!r}")
    return number


def load_csv_graph(root: Path, *, require_complete: bool = False) -> Graph:
    """Load declared typed vertices, then directed edges, preserving duplicates.

    File order is lexicographic by relative path, and row order is retained.
    Initial static/ and dynamic/ directories must both exist.  Root-level
    updateStream*.csv and non-CSV sidecars do not belong to the initial graph.
    With require_complete, all 8 node, 23 edge and 2 auxiliary families must be
    present; header-only files count as present.
    """
    root = Path(root).resolve()
    files = _discover(root, require_complete)
    # Each stored edge points at these canonical tuple objects.  In particular,
    # repeated high integer IDs do not allocate one tuple/int pair per endpoint.
    vertices: dict[Vertex, Vertex] = {}
    for path, family in files:
        if family not in NODE_FILES:
            continue
        vertex_type = NODE_FILES[family][1]
        for line, row in _rows(path, ("id",)):
            vertex = (vertex_type, _raw_id(row[0], path, line))
            if vertex in vertices:
                raise ValueError(f"{path}:{line}: duplicate typed vertex {vertex!r}")
            vertices[vertex] = vertex

    def declared(vertex_type: str, value: str, path: Path, line: int) -> Vertex:
        vertex = (vertex_type, _raw_id(value, path, line))
        try:
            return vertices[vertex]
        except KeyError as error:
            raise ValueError(f"{path}:{line}: endpoint has no declared vertex: {vertex!r}") from error

    edges: list[Edge] = []
    for path, family in files:
        if family in EDGE_FILES:
            _, relation, source_type, target_type = EDGE_FILES[family]
            leading = (source_type + ".id", target_type + ".id")
            for line, row in _rows(path, leading):
                source = declared(source_type, row[0], path, line)
                target = declared(target_type, row[1], path, line)
                edges.append(Edge(source, target, relation))
        elif family in AUX_FILES:
            attribute = AUX_FILES[family][1]
            for line, row in _rows(path, ("Person.id", attribute), auxiliary=True):
                declared("Person", row[0], path, line)
    return Graph(frozenset(vertices), tuple(edges))
