#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.graphio import open_graph_reader


def main() -> int:
    parser = argparse.ArgumentParser(description="Select reproducible BFS source vertices from a CPGDB graph file.")
    parser.add_argument("--graph", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--node-type", default="Person")
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    if args.count <= 0:
        parser.error("--count must be positive")

    # Reservoir sampling keeps memory bounded by --count even for large SNB scale factors.
    rng = random.Random(args.seed)
    selected = []
    candidate_count = 0
    for node in open_graph_reader(args.graph).iter_nodes():
        if node.type != args.node_type:
            continue
        candidate_count += 1
        if len(selected) < args.count:
            selected.append(node)
            continue
        replacement = rng.randrange(candidate_count)
        if replacement < args.count:
            selected[replacement] = node

    if not selected:
        raise SystemExit(f"No nodes of type {args.node_type} in {args.graph}")
    rng.shuffle(selected)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["uuid", "type", "source", "ldbc_id"])
        for node in selected:
            writer.writerow([node.uuid, node.type, node.source, node.properties.get("ldbc_id", "")])
    print(f"Wrote {len(selected)} BFS roots to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
