#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.graphio import open_graph_reader
from tools.graphio.id_codec import (
    BackendLayout,
    parse_global_id,
    read_native_node_id_map,
    resolve_native_node_id,
)
from tools.ldbc_snb.convert import Placement, csv_rows, discover_csv_files
from tools.ldbc_snb.spec import EDGE_SPECS, NODE_SPECS


class Graph:
    def __init__(self) -> None:
        # Keep only adjacency: the reference BFS does not need properties.
        self.adj: dict[int, list[int]] = {}

    def add_node(self, node) -> None:
        self.add_node_uuid(node.uuid)

    def add_node_uuid(self, node_uuid: int) -> None:
        self.adj.setdefault(node_uuid, [])

    def add_edge(self, from_uuid: int, to_uuid: int, *, undirected: bool = True) -> None:
        self.adj.setdefault(from_uuid, []).append(to_uuid)
        self.adj.setdefault(to_uuid, [])
        if undirected:
            self.adj[to_uuid].append(from_uuid)

    def bfs_depths(self, start_uuid: int, max_depth: int | None = None) -> dict[int, int]:
        if start_uuid not in self.adj:
            raise ValueError(f"Start node {start_uuid} not found in graph")
        visited = {start_uuid}
        depths = {start_uuid: 0}
        queue = deque([start_uuid])
        while queue:
            current = queue.popleft()
            if max_depth is not None and depths[current] >= max_depth:
                continue
            for neighbor in self.adj[current]:
                if neighbor not in visited:
                    visited.add(neighbor)
                    depths[neighbor] = depths[current] + 1
                    queue.append(neighbor)
        return depths


def load_graph(path: str | Path, *, undirected: bool = True) -> Graph:
    reader = open_graph_reader(path)
    graph = Graph()
    for node in reader.iter_nodes():
        graph.add_node(node)
    for relationship in reader.iter_relationships():
        graph.add_edge(relationship.from_uuid, relationship.to_uuid, undirected=undirected)
    return graph


def placement_from_schema(schema_path: str | Path) -> Placement:
    """Reconstruct schema-based authoritative placement without reading graph.jsonl.

    The generated LDBC graph-shape schema contains the authoritative source for
    every node and relationship type.  That is sufficient for schema-based data
    because PolyGraph's owner bits are backend-name based.  A genuinely
    per-object distributed placement cannot be reconstructed from the schema
    alone, so fail loudly rather than generating incorrect reference IDs.
    """
    path = Path(schema_path).expanduser().resolve()
    raw = json.loads(path.read_text(encoding="utf-8"))

    node_sources: dict[str, str] = {}
    relationship_sources: dict[str, str] = {}
    used_sources: set[str] = set()
    distributed: list[str] = []

    def source_for(record: dict, kind: str) -> str:
        name = str(record.get("name", "<unknown>"))
        source = str(record.get("source", "")).strip()
        if not source or source == "Distributed":
            distributed.append(f"{kind}:{name}")
            return ""
        used_sources.add(source)
        return source

    for record in raw.get("nodeTypes", []):
        source = source_for(record, "node")
        if source:
            node_sources[str(record["name"])] = source

    for record in raw.get("relations", []):
        source = source_for(record, "relationship")
        if source:
            relationship_sources[str(record["name"])] = source

    if distributed:
        examples = ", ".join(distributed[:5])
        suffix = " ..." if len(distributed) > 5 else ""
        raise ValueError(
            "Raw-CSV reference BFS cannot reconstruct per-object distributed placement "
            f"from the graph schema alone ({examples}{suffix}). Use a schema-based domain "
            "with explicit type placement or extend the reference configuration with the "
            "original placement routing order."
        )
    if not used_sources:
        raise ValueError(f"Graph schema contains no authoritative placement sources: {path}")

    layout = BackendLayout.from_sources(used_sources)
    return Placement(
        storage_backends=layout.backends,
        node_sources=node_sources,
        relationship_sources=relationship_sources,
    )


def load_ldbc_snb_graph(
    input_dir: str | Path,
    *,
    schema_path: str | Path,
    native_id_map_path: str | Path | None = None,
    undirected: bool = True,
) -> Graph:
    """Load an LDBC SNB CsvBasic snapshot directly into reference adjacency.

    No PolyGraph JSON/JSONL data is read.  IDs are regenerated using the same
    bounded-memory node payload allocation as the LDBC converter.  Nodes must be
    visited before relationships because the converter assigns compact prefix
    codes in that deterministic order as well.
    """
    source = Path(input_dir).expanduser().resolve()
    if not source.is_dir():
        raise FileNotFoundError(f"LDBC CsvBasic input directory does not exist: {source}")

    grouped = discover_csv_files(source)
    placement = placement_from_schema(schema_path)
    native_ids = read_native_node_id_map(native_id_map_path)
    graph = Graph()

    # First pass: reproduce the converter's deterministic node-prefix allocation.
    for prefix, spec in NODE_SPECS.items():
        for path in grouped.get(prefix, []):
            for _header, row in csv_rows(path):
                if not row:
                    continue
                external_id = row[0].strip()
                if not external_id:
                    continue
                node_uuid, _ = placement.node_route(spec.type_name, external_id)
                graph.add_node_uuid(resolve_native_node_id(node_uuid, native_ids))

    # Second pass: construct adjacency directly from the original relationships.
    for prefix, spec in EDGE_SPECS.items():
        for path in grouped.get(prefix, []):
            for _header, row in csv_rows(path):
                if len(row) < 2:
                    continue
                from_id = row[0].strip()
                to_id = row[1].strip()
                if not from_id or not to_id:
                    continue
                from_uuid, _ = placement.node_route(spec.from_type, from_id)
                to_uuid, _ = placement.node_route(spec.to_type, to_id)
                graph.add_edge(
                    resolve_native_node_id(from_uuid, native_ids),
                    resolve_native_node_id(to_uuid, native_ids),
                    undirected=undirected,
                )

    return graph


def save_depths_to_csv(depths: dict[int, int], path: str | Path) -> None:
    with Path(path).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter=";")
        writer.writerow(["vertex_id", "distance"])
        for node_uuid, depth in sorted(depths.items(), key=lambda item: (item[1], item[0])):
            writer.writerow([node_uuid, depth])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run reference BFS over PolyGraph graph data or original LDBC SNB CsvBasic files."
    )
    parser.add_argument("--input", help="Path to .json/.jsonl PolyGraph graph data")
    parser.add_argument("--input-json", dest="input_json", help="Backward-compatible alias for --input")
    parser.add_argument("--ldbc-input-dir", help="Original extracted LDBC CsvBasic directory containing static/ and dynamic/")
    parser.add_argument("--schema", help="Generated graph-shape schema; required with --ldbc-input-dir")
    parser.add_argument(
        "--native-id-map",
        help="Optional native node-ID map from the loaded domain; applied after reconstructing CSV IDs",
    )
    parser.add_argument("--output-csv", required=True)
    parser.add_argument("--start-uuid", required=True, type=parse_global_id)
    parser.add_argument("--max-depth", type=int, default=7, help="Use -1 for unlimited")
    parser.add_argument("--directed", action="store_true", help="Treat relationships as directed")
    args = parser.parse_args()

    args.input_path = args.input or args.input_json
    if bool(args.input_path) == bool(args.ldbc_input_dir):
        parser.error("choose exactly one reference source: --input/--input-json or --ldbc-input-dir")
    if args.ldbc_input_dir and not args.schema:
        parser.error("--schema is required with --ldbc-input-dir")
    return args


def main() -> int:
    args = parse_args()
    if args.ldbc_input_dir:
        graph = load_ldbc_snb_graph(
            args.ldbc_input_dir,
            schema_path=args.schema,
            native_id_map_path=args.native_id_map,
            undirected=not args.directed,
        )
    else:
        graph = load_graph(args.input_path, undirected=not args.directed)

    max_depth = None if args.max_depth < 0 else args.max_depth
    depths = graph.bfs_depths(args.start_uuid, max_depth=max_depth)
    save_depths_to_csv(depths, args.output_csv)
    print(f"BFS completed from {args.start_uuid}; wrote {len(depths)} rows to {args.output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
