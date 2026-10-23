#!/usr/bin/env python3
"""Run isolated PolyGraph cache benchmarks across Arango concurrency/batch settings."""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]


def _positive_csv(value: str) -> list[int]:
    result: list[int] = []
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        parsed = int(token)
        if parsed <= 0:
            raise argparse.ArgumentTypeError("sweep values must be greater than zero")
        if parsed not in result:
            result.append(parsed)
    if not result:
        raise argparse.ArgumentTypeError("at least one sweep value is required")
    return result


def _read_comparison(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _write_sweep_plan(
    source: Path,
    destination: Path,
    *,
    concurrency: int,
    batch_size: int,
) -> None:
    raw = json.loads(source.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("HTTP request plan root must be an object")

    defaults = raw.setdefault("defaults", {})
    if not isinstance(defaults, dict):
        raise ValueError("HTTP request plan defaults must be an object")
    payload = defaults.setdefault("payload", {})
    if not isinstance(payload, dict):
        raise ValueError("HTTP request plan defaults.payload must be an object")

    # Request-plan payload values override command-line/base payload values in
    # benchmark_cache_configs.py. Modify the generated plan itself so the sweep
    # cannot be accidentally masked by values such as the SF1 example's
    # neighborBatchSize=250000 and arangoMaxConcurrentRequests=8.
    payload["arangoMaxConcurrentRequests"] = concurrency
    payload["neighborBatchSize"] = batch_size

    # Per-request payloads have even higher precedence. Remove only the two
    # parameters under test so every request in this sweep uses the same pair.
    requests = raw.get("requests", [])
    if isinstance(requests, list):
        for request in requests:
            if not isinstance(request, dict):
                continue
            request_payload = request.get("payload")
            if isinstance(request_payload, dict):
                request_payload.pop("arangoMaxConcurrentRequests", None)
                request_payload.pop("neighborBatchSize", None)

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(raw, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run one isolated benchmark-caches invocation per Arango concurrency / "
            "neighbor batch-size pair and aggregate comparison.csv results."
        )
    )
    parser.add_argument("domain", help="Domain ID or domain.json path accepted by tools/polygraph.py")
    cache_group = parser.add_mutually_exclusive_group(required=True)
    cache_group.add_argument("--cache-configs", nargs="+")
    cache_group.add_argument("--cache-plans", nargs="+")
    parser.add_argument("--http-request-plan", required=True)
    parser.add_argument("--cache-root")
    parser.add_argument("--planning-db")
    parser.add_argument("--http-server-config")
    parser.add_argument("--base-output-dir", required=True)
    parser.add_argument("--concurrency", type=_positive_csv, default=_positive_csv("4,8,16,32"))
    parser.add_argument("--batch-sizes", type=_positive_csv, default=_positive_csv("5000,10000,20000,50000"))
    parser.add_argument(
        "--overwrite-existing",
        action="store_true",
        help="Pass --overwrite when a sweep output directory already exists.",
    )
    parser.add_argument(
        "--keep-going",
        action="store_true",
        help="Continue with later sweep points after a failed benchmark.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands without executing them.",
    )
    parser.add_argument(
        "bfs_args",
        nargs=argparse.REMAINDER,
        help="Additional BFS arguments after --, e.g. -- --no-predecessors.",
    )
    args = parser.parse_args()

    bfs_args = list(args.bfs_args)
    if bfs_args and bfs_args[0] == "--":
        bfs_args.pop(0)

    source_plan = Path(args.http_request_plan).expanduser().resolve()
    if not source_plan.is_file():
        raise FileNotFoundError(source_plan)

    base_output = Path(args.base_output_dir).expanduser().resolve()
    base_output.mkdir(parents=True, exist_ok=True)
    generated_plan_dir = base_output / "_request_plans"
    aggregate: list[dict[str, Any]] = []
    failures = 0

    for concurrency in args.concurrency:
        for batch_size in args.batch_sizes:
            label = f"arango-c{concurrency}-b{batch_size}"
            output_dir = base_output / label
            sweep_plan = generated_plan_dir / f"{label}.json"
            _write_sweep_plan(
                source_plan,
                sweep_plan,
                concurrency=concurrency,
                batch_size=batch_size,
            )

            command: list[str] = [
                sys.executable,
                str(ROOT / "tools" / "polygraph.py"),
                "benchmark-caches",
                args.domain,
                "--transport",
                "http",
                "--output-dir",
                str(output_dir),
                "--http-request-plan",
                str(sweep_plan),
            ]
            if args.cache_configs:
                command.extend(["--cache-configs", *args.cache_configs])
            else:
                command.extend(["--cache-plans", *args.cache_plans])
            if args.cache_root:
                command.extend(["--cache-root", args.cache_root])
            if args.planning_db:
                command.extend(["--planning-db", args.planning_db])
            if args.http_server_config:
                command.extend(["--http-server-config", args.http_server_config])
            if args.overwrite_existing:
                command.append("--overwrite")
            if bfs_args:
                command.extend(["--", *bfs_args])

            print("\n[" + label + "]")
            print("request plan:", sweep_plan)
            print(" ".join(command), flush=True)
            if args.dry_run:
                continue

            completed = subprocess.run(command, cwd=ROOT, check=False)
            if completed.returncode != 0:
                failures += 1
                aggregate.append(
                    {
                        "sweep": label,
                        "arango_max_concurrent_requests": concurrency,
                        "neighbor_batch_size": batch_size,
                        "status": "benchmark-failed",
                        "returncode": completed.returncode,
                    }
                )
                if not args.keep_going:
                    break
                continue

            comparison_path = output_dir / "comparison.csv"
            rows = _read_comparison(comparison_path)
            if not rows:
                failures += 1
                aggregate.append(
                    {
                        "sweep": label,
                        "arango_max_concurrent_requests": concurrency,
                        "neighbor_batch_size": batch_size,
                        "status": "missing-comparison",
                    }
                )
                if not args.keep_going:
                    break
                continue

            for row in rows:
                aggregate.append(
                    {
                        "sweep": label,
                        "arango_max_concurrent_requests": concurrency,
                        "neighbor_batch_size": batch_size,
                        **row,
                    }
                )
        if failures and not args.keep_going:
            break

    if args.dry_run:
        return 0

    json_path = base_output / "arango-sweep.json"
    csv_path = base_output / "arango-sweep.csv"
    json_path.write_text(json.dumps(aggregate, indent=2) + "\n", encoding="utf-8")

    fields: list[str] = []
    for row in aggregate:
        for key in row:
            if key not in fields:
                fields.append(key)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(aggregate)

    print(f"\nSweep CSV:  {csv_path}")
    print(f"Sweep JSON: {json_path}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
