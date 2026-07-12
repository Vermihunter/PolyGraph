#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.measurement.graph_bench.config import load_config
from tools.measurement.graph_bench.graph_data import read_start_uuids
from tools.measurement.graph_bench.orchestrator import BfsBenchmarkOrchestrator


def main() -> int:
    parser = argparse.ArgumentParser(description="Run distributed graph_db BFS for many start UUIDs and compare each output to tools/algorithms/bfs.py.")
    parser.add_argument("--config")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--start-uuid", action="append", default=[], help="Can be repeated or comma-separated.")
    parser.add_argument("--start-uuids-file")
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--reload-each-repetition", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    start_uuids = read_start_uuids(args.start_uuid, args.start_uuids_file)
    if not start_uuids:
        raise SystemExit("Provide at least one --start-uuid or --start-uuids-file")

    config = load_config(args.config)
    orchestrator = BfsBenchmarkOrchestrator(config, args.output_dir, dry_run=args.dry_run)
    try:
        orchestrator.run_distributed(
            start_uuids,
            args.repetitions,
            compare=True,
            reload_each_repetition=args.reload_each_repetition,
        )
    finally:
        csv_path, jsonl_path, summary_path = orchestrator.flush()
        print(f"Measurements: {csv_path}")
        print(f"JSONL:        {jsonl_path}")
        print(f"Summary:      {summary_path}")

    failures = [r for r in orchestrator.sink.records if r.phase == "compare_reference" and r.same_as_reference is False]
    if failures:
        print(f"FAILED: {len(failures)} BFS output comparison(s) failed. See {orchestrator.diffs_dir}", file=sys.stderr)
        return 1
    print("OK: every BFS output matched tools/algorithms/bfs.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
