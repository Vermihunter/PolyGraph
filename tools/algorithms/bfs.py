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
from tools.graphio.id_codec import parse_global_id


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


def placement_from_schema(schema_path: str | Path):
    """Compatibility alias; LDBC reconstruction belongs to the support package."""
    from tools.ldbc_snb.reference_bfs import placement_from_schema as implementation
    return implementation(schema_path)


def load_ldbc_snb_graph(input_dir: str | Path, *, schema_path: str | Path,
                        native_id_map_path: str | Path | None = None, undirected: bool = True) -> Graph:
    """Compatibility alias for the LDBC reference adapter."""
    from tools.ldbc_snb.reference_bfs import load_ldbc_snb_graph as implementation
    return implementation(input_dir, schema_path=schema_path, native_id_map_path=native_id_map_path,
                          undirected=undirected, graph_factory=Graph)


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
    parser.add_argument("--graph-manifest", "--domain", dest="graph_manifest", help="Graph manifest, directory or domain ID; uses normalized data.")
    parser.add_argument("--data-root", help="Search directory for graph IDs.")
    parser.add_argument("--input", help="Path to .json/.jsonl PolyGraph graph data")
    parser.add_argument("--input-json", dest="input_json", help="Backward-compatible alias for --input")
    parser.add_argument("--ldbc-input-dir", help="Original extracted LDBC CsvBasic directory containing static/ and dynamic/")
    parser.add_argument("--schema", help="Generated graph-shape schema; required with --ldbc-input-dir")
    parser.add_argument(
        "--native-id-map",
        help="Optional native node-ID map from the loaded domain; applied after reconstructing CSV IDs",
    )
    parser.add_argument("--output-csv", required=True)
    parser.add_argument("--start-uuid", type=parse_global_id)
    parser.add_argument("--max-depth", type=int, help="Defaults to graph config or 7; use -1 for unlimited")
    direction = parser.add_mutually_exclusive_group()
    direction.add_argument("--directed", dest="directed", action="store_const", const=True, default=None,
                           help="Treat relationships as directed; overrides graph default.")
    direction.add_argument("--undirected", dest="directed", action="store_const", const=False,
                           help="Traverse both ways; overrides graph default.")
    args = parser.parse_args()

    args.input_path = args.input or args.input_json
    if args.input and args.input_json:
        parser.error("choose either --input or legacy --input-json")
    if args.graph_manifest:
        if args.input_path or args.ldbc_input_dir:
            parser.error("--graph-manifest cannot be combined with input aliases")
        from tools.graphio.domain import resolve_domain
        domain = resolve_domain(args.graph_manifest, args.data_root)
        args.input_path = str(domain.paths.data)
        if args.max_depth is None:
            args.max_depth = domain.max_depth
        if args.directed is None:
            if domain.direction not in {"directed", "undirected", "both"}:
                parser.error("domain BFS direction must be directed, undirected or both")
            args.directed = domain.direction == "directed"
        if args.start_uuid is None:
            if domain.start_uuid is None:
                parser.error("Reference BFS requires a canonical --start-uuid; use polygraph.py run for typed --start-source roots")
            args.start_uuid = parse_global_id(domain.start_uuid)
    if args.max_depth is None:
        args.max_depth = 7
    if args.directed is None:
        args.directed = False
    if args.start_uuid is None:
        parser.error("provide --start-uuid or a graph manifest with startUuid")
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
