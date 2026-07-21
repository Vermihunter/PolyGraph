#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.measurement.graph_bench.config import load_config
from tools.measurement.graph_bench.graph_data import GraphData, read_start_uuids
from tools.measurement.graph_bench.orchestrator import BfsBenchmarkOrchestrator, normalize_backends


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Measure BFS loading/query performance for native DB backends, graph_db full-backend modes, and distributed graph_db."
    )
    parser.add_argument("mode", choices=["native", "implementation-full", "distributed", "all"])
    parser.add_argument("--config", help="JSON config file. Defaults are project-local paths and localhost DB credentials.")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--start-uuid", action="append", default=[], help="Can be repeated or comma-separated.")
    parser.add_argument("--start-uuids-file", help="Text file with one uint64 ID per line or comma-separated IDs.")
    parser.add_argument("--native-backends", default="neo4j,arango")
    parser.add_argument("--implementation-backends", default="neo4j,arango,gpu")
    parser.add_argument("--no-compare", action="store_true", help="Do not compare outputs to tools/algorithms/bfs.py")
    parser.add_argument("--reload-each-repetition", action="store_true", help="Reload data before every repetition instead of once per scenario/system.")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    config = load_config(args.config)
    start_uuids = read_start_uuids(args.start_uuid, args.start_uuids_file)
    if not start_uuids:
        graph = GraphData.load(config.paths.data)
        if not graph.nodes:
            raise SystemExit("No start ID supplied and data file contains no nodes")
        start_uuids = [graph.nodes[0].uuid]
        print(f"No --start-uuid supplied; using first data node: {start_uuids[0]}", file=sys.stderr)

    orchestrator = BfsBenchmarkOrchestrator(config, args.output_dir, dry_run=args.dry_run, verbose=not args.quiet)
    compare = not args.no_compare
    native_backends = normalize_backends(args.native_backends.split(","))
    implementation_backends = normalize_backends(args.implementation_backends.split(","))

    try:
        if args.mode == "native":
            orchestrator.run_native_full(
                start_uuids,
                args.repetitions,
                native_backends,
                compare=compare,
                reload_each_repetition=args.reload_each_repetition,
            )
        elif args.mode == "implementation-full":
            orchestrator.run_implementation_full(
                start_uuids,
                args.repetitions,
                implementation_backends,
                compare=compare,
                reload_each_repetition=args.reload_each_repetition,
            )
        elif args.mode == "distributed":
            orchestrator.run_distributed(
                start_uuids,
                args.run_id,
                args.repetitions,
                compare=compare,
                reload_each_repetition=args.reload_each_repetition,
            )
        else:
            orchestrator.run_all(
                start_uuids,
                args.repetitions,
                native_backends=native_backends,
                implementation_backends=implementation_backends,
                compare=compare,
                reload_each_repetition=args.reload_each_repetition,
            )
    finally:
        csv_path, jsonl_path, summary_path = orchestrator.flush()
        print(f"Measurements: {csv_path}")
        print(f"JSONL:        {jsonl_path}")
        print(f"Summary:      {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
