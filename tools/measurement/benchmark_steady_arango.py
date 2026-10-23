#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from statistics import mean
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
WATCHER = ROOT / "tools" / "measurement" / "arango_watch.py"


def _safe(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip("-.")
    return value or "request"


def _resolve_existing_plan(cache_root: Path, config: str | Path) -> Path:
    config_path = Path(config).expanduser().resolve()
    candidate = cache_root / config_path.stem / "cache-plan.json"
    if not candidate.is_file():
        raise FileNotFoundError(
            f"Existing cache plan not found for {config_path.name}: {candidate}\n"
            "This benchmark intentionally uses --cache-plans so cache planning is skipped."
        )
    return candidate.resolve()


def _resolve_inputs(args: argparse.Namespace) -> list[Path]:
    if args.cache_plans:
        plans = [Path(value).expanduser().resolve() for value in args.cache_plans]
        missing = [path for path in plans if not path.is_file()]
        if missing:
            raise FileNotFoundError("Missing cache plan(s):\n  " + "\n  ".join(map(str, missing)))
        return plans

    if not args.cache_configs:
        raise ValueError("Either --cache-plans or --cache-configs is required")
    cache_root = Path(args.cache_root).expanduser().resolve()
    return [_resolve_existing_plan(cache_root, value) for value in args.cache_configs]


def _load_plan(path: Path) -> dict[str, Any]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"Request plan root must be an object: {path}")
    items = raw.get("requests")
    if not isinstance(items, list) or not items:
        raise ValueError(f"Request plan has no requests: {path}")
    return raw


def _resolved_start(item: dict[str, Any], defaults: dict[str, Any]) -> str:
    return str(item.get("startUuid", defaults.get("startUuid", "$DOMAIN_START_UUID")))


def _steady_state_plan(
    source_plan: Path,
    *,
    warmup_rounds: int,
    warmup_roots: str,
) -> tuple[dict[str, Any], list[str]]:
    raw = _load_plan(source_plan)
    defaults = dict(raw.get("defaults") or {})
    items = [dict(item) for item in raw["requests"] if isinstance(item, dict)]

    measured = [
        item for item in items
        if bool(item.get("measure", True))
    ]
    if not measured:
        raise ValueError("Request plan contains no measured requests")

    root_source = measured if warmup_roots == "measured" else items
    representative: dict[str, dict[str, Any]] = {}
    order: list[str] = []
    for item in root_source:
        start = _resolved_start(item, defaults)
        if start not in representative:
            representative[start] = item
            order.append(start)

    warmups: list[dict[str, Any]] = []
    for round_index in range(1, warmup_rounds + 1):
        for root_index, start in enumerate(order, start=1):
            template = representative[start]
            request: dict[str, Any] = {
                "id": f"warmup-r{round_index:02d}-{root_index:02d}-{_safe(str(template.get('id', start)))}",
                "startUuid": start,
                "measure": False,
            }
            if isinstance(template.get("payload"), dict):
                request["payload"] = dict(template["payload"])
            warmups.append(request)

    clean_measured: list[dict[str, Any]] = []
    for item in measured:
        item = dict(item)
        item.pop("atMs", None)
        item["measure"] = True
        clean_measured.append(item)

    result = dict(raw)
    result["mode"] = "serial"
    result["requests"] = warmups + clean_measured
    result["steadyStateWarmup"] = {
        "rounds": warmup_rounds,
        "rootPolicy": warmup_roots,
        "roots": order,
        "originalPlan": str(source_plan),
    }
    return result, order


def _find_measurements(output_dir: Path) -> Path | None:
    candidates = list(output_dir.rglob("measurements.csv"))
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime_ns)


def _analyze_warmup(measurements: Path, output: Path) -> dict[str, Any]:
    with measurements.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle, delimiter=";"))

    rounds: dict[int, list[float]] = {}
    per_root: dict[str, dict[int, float]] = {}
    measured_values: list[float] = []

    pattern = re.compile(r"^warmup-r(\d+)-\d+-(.+)$")
    for row in rows:
        if row.get("status") != "success" or not row.get("server_bfs_ms"):
            continue
        duration = float(row["server_bfs_ms"])
        request_id = row.get("request_id", "")
        match = pattern.match(request_id)
        if match:
            round_index = int(match.group(1))
            root = row.get("start_uuid", "")
            rounds.setdefault(round_index, []).append(duration)
            per_root.setdefault(root, {})[round_index] = duration
        elif str(row.get("measured", "true")).lower() not in {"false", "0", "no"}:
            measured_values.append(duration)

    round_stats = {
        str(index): {
            "count": len(values),
            "mean_server_bfs_ms": mean(values) if values else None,
            "min_server_bfs_ms": min(values) if values else None,
            "max_server_bfs_ms": max(values) if values else None,
        }
        for index, values in sorted(rounds.items())
    }

    root_stats: dict[str, Any] = {}
    for root, values in per_root.items():
        ordered = sorted(values.items())
        entry: dict[str, Any] = {
            "rounds": {str(index): value for index, value in ordered},
        }
        if len(ordered) >= 2:
            first = ordered[-2][1]
            last = ordered[-1][1]
            entry["last_over_previous"] = last / first if first else None
            entry["improvement_percent"] = ((first - last) / first * 100.0) if first else None
        root_stats[root] = entry

    diagnosis: list[str] = []
    if len(rounds) >= 2:
        last_two = sorted(rounds)[-2:]
        previous = mean(rounds[last_two[0]])
        latest = mean(rounds[last_two[1]])
        ratio = latest / previous if previous else 1.0
        if ratio < 0.35:
            diagnosis.append(
                "Strong warm-cache effect: the latest warmup round is below 35% of the previous round."
            )
        elif ratio > 0.80 and latest > 30_000:
            diagnosis.append(
                "Warmup is not converging: repeated representative roots remain slow. "
                "This is consistent with cache-capacity thrashing or a backend/storage bottleneck."
            )
        elif ratio > 0.90:
            diagnosis.append("Warmup rounds are stable; steady-state appears reached.")
        else:
            diagnosis.append("Warmup is still changing materially; consider one additional warmup round.")

    result = {
        "measurements": str(measurements),
        "rounds": round_stats,
        "roots": root_stats,
        "measured_mean_server_bfs_ms": mean(measured_values) if measured_values else None,
        "diagnosis": diagnosis,
    }
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    md = ["# Warmup analysis", ""]
    for index, stats in round_stats.items():
        md.append(
            f"- round {index}: mean={stats['mean_server_bfs_ms']:.3f} ms, "
            f"min={stats['min_server_bfs_ms']:.3f} ms, max={stats['max_server_bfs_ms']:.3f} ms"
        )
    if measured_values:
        md.append(f"- measured mean: {mean(measured_values):.3f} ms")
    md.append("")
    md.append("## Diagnosis")
    md.extend(f"- {item}" for item in diagnosis or ["No multi-round diagnosis available."])
    output.with_suffix(".md").write_text("\n".join(md) + "\n", encoding="utf-8")
    return result


def _stop_process(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run cache benchmarks from already-generated cache-plan.json files, "
            "prepend reproducible representative-root warmup rounds, and collect "
            "ArangoDB process/cache/I/O diagnostics."
        )
    )
    parser.add_argument("--domain", required=True)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--cache-plans", nargs="+")
    inputs.add_argument(
        "--cache-configs",
        nargs="+",
        help="Config filenames used only to locate <cache-root>/<config-stem>/cache-plan.json; planning is skipped.",
    )
    parser.add_argument("--cache-root", default="data/ldbc-snb/sf1_0/schema-based/caches")
    parser.add_argument("--request-plan", required=True)
    parser.add_argument("--warmup-rounds", type=int, default=2)
    parser.add_argument("--warmup-roots", choices=["measured", "all"], default="measured")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--arango-container")
    parser.add_argument("--arango-database", default="ldbc_snb_sf1_0")
    parser.add_argument("--arango-user", default="root")
    parser.add_argument(
        "--arango-password",
        default=os.environ.get("ARANGO_ROOT_PASSWORD", "rootpassword"),
    )
    parser.add_argument("--watch-interval", type=float, default=1.0)
    parser.add_argument("--http-server-config")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--http-startup-timeout", type=float, default=0.0)
    parser.add_argument("--http-request-timeout", type=float, default=0.0)
    parser.add_argument("--skip-redis-load", action="store_true")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    if args.warmup_rounds < 0:
        raise ValueError("--warmup-rounds must be >= 0")
    if args.watch_interval <= 0:
        raise ValueError("--watch-interval must be > 0")

    plans = _resolve_inputs(args)
    output_root = Path(args.output_dir).expanduser().resolve()
    if output_root.exists() and args.overwrite:
        shutil.rmtree(output_root)
    elif output_root.exists() and any(output_root.iterdir()):
        raise ValueError(f"Output directory is not empty: {output_root}; use --overwrite")
    output_root.mkdir(parents=True, exist_ok=True)

    source_plan = Path(args.request_plan).expanduser().resolve()
    generated_plan, roots = _steady_state_plan(
        source_plan,
        warmup_rounds=args.warmup_rounds,
        warmup_roots=args.warmup_roots,
    )
    plan_dir = output_root / "_benchmark"
    plan_dir.mkdir(parents=True, exist_ok=True)
    steady_plan = plan_dir / "steady-state-request-plan.json"
    steady_plan.write_text(json.dumps(generated_plan, indent=2) + "\n", encoding="utf-8")

    print(
        f"[steady-benchmark] existing-plan fast path: {len(plans)} cache plan(s); "
        "cache-plan.sqlite3 planning will NOT run",
        flush=True,
    )
    print(
        f"[steady-benchmark] warmup: {args.warmup_rounds} round(s) x "
        f"{len(roots)} representative root(s), all unmeasured",
        flush=True,
    )
    print(f"[steady-benchmark] generated request plan: {steady_plan}", flush=True)

    overall_status = 0
    manifest: list[dict[str, Any]] = []

    for index, cache_plan in enumerate(plans, start=1):
        cache_name = cache_plan.parent.name
        run_root = output_root / f"{index:02d}-{cache_name}"
        diagnostics = run_root / "arango-diagnostics"
        benchmark_output = run_root / "benchmark"
        diagnostics.mkdir(parents=True, exist_ok=True)

        watcher_command = [
            sys.executable,
            str(WATCHER),
            "watch",
            "--output-dir",
            str(diagnostics),
            "--database",
            args.arango_database,
            "--user",
            args.arango_user,
            "--password",
            args.arango_password,
            "--interval",
            str(args.watch_interval),
        ]
        if args.arango_container:
            watcher_command.extend(["--container", args.arango_container])

        print(f"\n[{index}/{len(plans)}] {cache_name}", flush=True)
        print(f"  cache plan:   {cache_plan}", flush=True)
        print("  planning DB:  SKIPPED", flush=True)
        print(f"  diagnostics:  {diagnostics}", flush=True)

        watcher = subprocess.Popen(watcher_command, cwd=ROOT)
        time.sleep(min(1.0, args.watch_interval))

        command = [
            sys.executable,
            str(ROOT / "tools" / "polygraph.py"),
            "benchmark-caches",
            args.domain,
            "--transport",
            "http",
            "--cache-plans",
            str(cache_plan),
            "--http-request-plan",
            str(steady_plan),
            "--http-port",
            str(args.http_port),
            "--http-startup-timeout",
            str(args.http_startup_timeout),
            "--http-request-timeout",
            str(args.http_request_timeout),
            "--output-dir",
            str(benchmark_output),
            "--overwrite",
        ]
        if args.http_server_config:
            command.extend(["--http-server-config", args.http_server_config])
        if args.skip_redis_load:
            command.append("--skip-redis-load")

        started = time.time_ns()
        try:
            completed = subprocess.run(command, cwd=ROOT, check=False)
        finally:
            _stop_process(watcher)
        finished = time.time_ns()

        measurements = _find_measurements(benchmark_output)
        warmup_analysis = None
        if measurements is not None:
            warmup_analysis = _analyze_warmup(
                measurements,
                diagnostics / "warmup-analysis.json",
            )
            subprocess.run(
                [
                    sys.executable,
                    str(WATCHER),
                    "analyze",
                    "--diagnostics",
                    str(diagnostics),
                    "--measurements",
                    str(measurements),
                ],
                cwd=ROOT,
                check=False,
            )

        item = {
            "cache": cache_name,
            "cachePlan": str(cache_plan),
            "benchmarkOutput": str(benchmark_output),
            "diagnostics": str(diagnostics),
            "startedUnixNs": started,
            "finishedUnixNs": finished,
            "returncode": completed.returncode,
            "warmupAnalysis": warmup_analysis,
        }
        manifest.append(item)
        if completed.returncode != 0:
            overall_status = completed.returncode or 1

    (output_root / "steady-benchmark-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )
    return overall_status


if __name__ == "__main__":
    raise SystemExit(main())
