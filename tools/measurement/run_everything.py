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
from tools.measurement.graph_bench.plotting import write_plots
from tools.measurement.graph_bench.services import start_services
from tools.measurement.graph_bench.subprocesses import CommandRunner


def main() -> int:
    parser = argparse.ArgumentParser(description="One-command benchmark: references + native full DB replicas + graph_db full backends + distributed graph_db + plots.")
    parser.add_argument("--config")
    parser.add_argument("--run-id", type=str, help="Run ID to identify this benchmark run in the output files.", required=True)
    parser.add_argument("--output-dir", default="tools/measurement/results/full_benchmark")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--start-uuid", action="append", default=[], help="Can be repeated or comma-separated.")
    parser.add_argument("--start-uuids-file")
    parser.add_argument("--native-backends", default="neo4j,arango")
    parser.add_argument("--implementation-backends", default="neo4j,arango,gpu")
    parser.add_argument("--reload-each-repetition", action="store_true")
    parser.add_argument("--no-compare", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-services", action="store_true", help="Run docker compose up -d before measuring and wait for Neo4j/ArangoDB HTTP endpoints.")
    parser.add_argument("--service-timeout", type=int, default=120)
    args = parser.parse_args()

    config = load_config(args.config)
    start_uuids = read_start_uuids(args.start_uuid, args.start_uuids_file)
    if not start_uuids:
        graph = GraphData.load(config.paths.data)
        if not graph.nodes:
            raise SystemExit("No start UUID supplied and data file contains no nodes")
        # Use a few deterministic nodes from different positions to avoid benchmarking only a tiny component.
        indexes = [0, len(graph.nodes) // 3, (2 * len(graph.nodes)) // 3]
        start_uuids = []
        for i in indexes:
            uuid = graph.nodes[i].uuid
            if uuid not in start_uuids:
                start_uuids.append(uuid)
        print("No start UUID supplied; using deterministic sample:", ", ".join(start_uuids), file=sys.stderr)

    if args.start_services:
        start_services(config, CommandRunner(ROOT, dry_run=args.dry_run), timeout_seconds=args.service_timeout)

    orchestrator = BfsBenchmarkOrchestrator(config, args.output_dir, dry_run=args.dry_run)
    try:
        orchestrator.run_all(
            start_uuids,
            args.run_id,
            args.repetitions,
            native_backends=normalize_backends(args.native_backends.split(",")),
            implementation_backends=normalize_backends(args.implementation_backends.split(",")),
            compare=not args.no_compare,
            reload_each_repetition=args.reload_each_repetition,
        )
    finally:
        csv_path, jsonl_path, summary_path = orchestrator.flush()
        print(f"Measurements: {csv_path}")
        print(f"JSONL:        {jsonl_path}")
        print(f"Summary:      {summary_path}")
        plot_dir = Path(args.output_dir) / "plots"
        for path in write_plots(csv_path, plot_dir):
            print(f"Plot:         {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
