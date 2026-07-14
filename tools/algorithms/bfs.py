#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.graphio import open_graph_reader


class Graph:
    def __init__(self) -> None:
        self.nodes: dict[str, object] = {}
        self.adj: dict[str, list[str]] = {}

    def add_node(self, node) -> None:
        self.nodes[node.uuid] = node
        self.adj.setdefault(node.uuid, [])

    def add_edge(self, from_uuid: str, to_uuid: str, *, undirected: bool = True) -> None:
        self.adj.setdefault(from_uuid, []).append(to_uuid)
        self.adj.setdefault(to_uuid, [])
        if undirected:
            self.adj[to_uuid].append(from_uuid)

    def bfs_depths(self, start_uuid: str, max_depth: int | None = None) -> dict[str, int]:
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


def load_graph(path: str, *, undirected: bool = True) -> Graph:
    reader = open_graph_reader(path)
    graph = Graph()
    for node in reader.iter_nodes():
        graph.add_node(node)
    for relationship in reader.iter_relationships():
        graph.add_edge(relationship.from_uuid, relationship.to_uuid, undirected=undirected)
    return graph


def save_depths_to_csv(depths: dict[str, int], path: str) -> None:
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter=";")
        writer.writerow(["vertex_id", "distance"])
        for node_uuid, depth in sorted(depths.items(), key=lambda item: (item[1], item[0])):
            writer.writerow([node_uuid, depth])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run reference BFS over CPGDB JSON or JSONL graph data.")
    parser.add_argument("--input", help="Path to .json/.jsonl graph data")
    parser.add_argument("--input-json", dest="input_json", help="Backward-compatible alias for --input")
    parser.add_argument("--output-csv", required=True)
    parser.add_argument("--start-uuid", required=True)
    parser.add_argument("--max-depth", type=int, default=7, help="Use -1 for unlimited")
    parser.add_argument("--directed", action="store_true", help="Treat relationships as directed")
    args = parser.parse_args()
    args.input_path = args.input or args.input_json
    if not args.input_path:
        parser.error("one of --input or --input-json is required")
    return args


def main() -> int:
    args = parse_args()
    graph = load_graph(args.input_path, undirected=not args.directed)
    max_depth = None if args.max_depth < 0 else args.max_depth
    depths = graph.bfs_depths(args.start_uuid, max_depth=max_depth)
    save_depths_to_csv(depths, args.output_csv)
    print(f"BFS completed from {args.start_uuid}; wrote {len(depths)} rows to {args.output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
