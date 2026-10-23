#!/usr/bin/env python3
"""Parallel-capable entry point for PolyGraph HTTP cache benchmarks.

This is intentionally a thin compatibility layer over benchmark_cache_configs.py:
serial plans use the existing implementation unchanged, while plans with
``"mode": "parallel"`` execute top-level BFS HTTP requests concurrently.
"""
from __future__ import annotations

import concurrent.futures
import csv
import json
import subprocess
import threading
import time
import urllib.parse
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from tools.measurement import benchmark_cache_configs as base


_ORIGINAL_LOAD_HTTP_REQUEST_PLAN = base._load_http_request_plan
_ORIGINAL_RUN_HTTP_BENCHMARK = base._run_http_benchmark
_ORIGINAL_SPLIT_SERVER_EVENTS = base._split_server_events_by_request


@dataclass(frozen=True)
class ParallelHttpRequestSpec:
    request_id: str
    start_uuid: str
    payload: dict[str, Any]
    at_ms: float | None = None
    repetition: int | None = None
    measure: bool = True
    mode: str = "parallel"
    max_concurrency: int | None = None


def _load_http_request_plan(
    path: str | Path,
    *,
    domain_start_uuid: str,
    base_payload: dict[str, Any],
) -> list[base.HttpRequestSpec | ParallelHttpRequestSpec]:
    source = Path(path).expanduser().resolve()
    raw = json.loads(source.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("HTTP request plan root must be a JSON object")

    mode = str(raw.get("mode", "serial")).strip().lower()
    if mode == "serial":
        return _ORIGINAL_LOAD_HTTP_REQUEST_PLAN(
            source,
            domain_start_uuid=domain_start_uuid,
            base_payload=base_payload,
        )
    if mode != "parallel":
        raise ValueError("HTTP request plan mode must be 'serial' or 'parallel'")

    raw_max_concurrency = raw.get("maxConcurrency")
    max_concurrency: int | None = None
    if raw_max_concurrency is not None:
        if isinstance(raw_max_concurrency, bool):
            raise ValueError("HTTP request plan maxConcurrency must be an integer")
        try:
            max_concurrency = int(raw_max_concurrency)
        except (TypeError, ValueError) as error:
            raise ValueError(
                "HTTP request plan maxConcurrency must be an integer"
            ) from error
        if max_concurrency <= 0:
            raise ValueError("HTTP request plan maxConcurrency must be greater than zero")

    defaults = raw.get("defaults", {})
    if defaults is None:
        defaults = {}
    if not isinstance(defaults, dict):
        raise ValueError("HTTP request plan defaults must be an object")
    default_payload = defaults.get("payload", {})
    if not isinstance(default_payload, dict):
        raise ValueError("HTTP request plan defaults.payload must be an object")
    default_start = str(defaults.get("startUuid", "$DOMAIN_START_UUID")).strip()

    items = raw.get("requests")
    if not isinstance(items, list) or not items:
        raise ValueError("HTTP request plan must contain a non-empty requests array")

    result: list[ParallelHttpRequestSpec] = []
    used_ids: set[str] = set()
    previous_at_ms = -1.0
    for index, item in enumerate(items, start=1):
        if not isinstance(item, dict):
            raise ValueError(
                f"HTTP request plan requests[{index - 1}] must be an object"
            )
        request_id = base._safe_component(str(item.get("id", f"request-{index:03d}")))
        if request_id in used_ids:
            raise ValueError(f"Duplicate HTTP request id: {request_id}")
        used_ids.add(request_id)

        start_uuid = str(item.get("startUuid", default_start)).strip()
        if start_uuid == "$DOMAIN_START_UUID":
            start_uuid = domain_start_uuid
        if not start_uuid:
            raise ValueError(f"HTTP request {request_id} has an empty startUuid")

        at_ms = float(item.get("atMs", 0.0))
        if at_ms < 0:
            raise ValueError(f"HTTP request {request_id} atMs must be non-negative")
        if at_ms < previous_at_ms:
            raise ValueError("HTTP request plan atMs values must be non-decreasing")
        previous_at_ms = at_ms

        request_payload = item.get("payload", {})
        if not isinstance(request_payload, dict):
            raise ValueError(f"HTTP request {request_id} payload must be an object")
        measure = item.get("measure", True)
        if not isinstance(measure, bool):
            raise ValueError(f"HTTP request {request_id} measure must be a JSON boolean")

        result.append(
            ParallelHttpRequestSpec(
                request_id=request_id,
                start_uuid=start_uuid,
                payload={**base_payload, **default_payload, **request_payload},
                at_ms=at_ms,
                measure=measure,
                max_concurrency=max_concurrency,
            )
        )
    return result


def _split_server_events_parallel_aware(
    server_events: Path,
    output_dir: Path,
    intervals: list[dict[str, Any]],
) -> dict[str, str]:
    ordered = sorted(intervals, key=lambda item: int(item["started_unix_ns"]))
    overlaps = any(
        int(current["started_unix_ns"]) <= int(previous["finished_unix_ns"])
        for previous, current in zip(ordered, ordered[1:])
    )
    if not overlaps:
        return _ORIGINAL_SPLIT_SERVER_EVENTS(server_events, output_dir, intervals)

    output_dir.mkdir(parents=True, exist_ok=True)
    for interval in ordered:
        (output_dir / f"{interval['request_id']}.events.jsonl").unlink(missing_ok=True)

    # Timestamps are enough to delimit each HTTP request for Prometheus, but not
    # enough to assign interleaved C++ events to one concurrent BFS. Preserve the
    # complete server log and explicitly mark per-request attribution unavailable.
    (output_dir / "index.json").write_text(
        json.dumps(
            [
                {
                    **interval,
                    "events_jsonl": None,
                    "event_attribution": "unavailable-overlapping-requests",
                }
                for interval in ordered
            ],
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return {}


def _run_http_benchmark(
    *,
    server_binary: Path,
    base_server_config: Path,
    domain_manifest: Path,
    observability_source: Path,
    cache_plan: Path,
    run_dir: Path,
    requests: list[base.HttpRequestSpec | ParallelHttpRequestSpec],
    cooldown_ms: int,
    host: str,
    port: int,
    startup_timeout: float,
    request_timeout: float,
    timeline: Path,
    cache_name: str,
) -> base.HttpBenchmarkResult:
    mode = getattr(requests[0], "mode", "serial") if requests else "serial"
    if mode != "parallel":
        return _ORIGINAL_RUN_HTTP_BENCHMARK(
            server_binary=server_binary,
            base_server_config=base_server_config,
            domain_manifest=domain_manifest,
            observability_source=observability_source,
            cache_plan=cache_plan,
            run_dir=run_dir,
            requests=requests,
            cooldown_ms=cooldown_ms,
            host=host,
            port=port,
            startup_timeout=startup_timeout,
            request_timeout=request_timeout,
            timeline=timeline,
            cache_name=cache_name,
        )

    if not 1 <= port <= 65535:
        raise ValueError("--http-port must be between 1 and 65535")
    if not requests:
        raise ValueError("Parallel HTTP benchmark requires at least one request")

    run_dir.mkdir(parents=True, exist_ok=True)
    observability = base._redirect_observability(observability_source, run_dir)
    server_config = base._http_server_config(
        base_server_config,
        domain=domain_manifest,
        cache_plan=cache_plan,
        observability=observability,
        host=host,
        port=port,
        output=run_dir / "server" / "drogon.json",
    )
    command = [str(server_binary), "--server-config", str(server_config)]
    (run_dir / "server" / "command.txt").write_text(
        " ".join(command) + "\n", encoding="utf-8"
    )
    max_concurrency = getattr(requests[0], "max_concurrency", None) or len(requests)
    (run_dir / "server" / "requests.json").write_text(
        json.dumps(
            [
                {
                    "id": request.request_id,
                    "atMs": request.at_ms,
                    "startUuid": request.start_uuid,
                    "payload": request.payload,
                    "repetition": request.repetition,
                    "measure": request.measure,
                    "mode": "parallel",
                    "maxConcurrency": max_concurrency,
                }
                for request in requests
            ],
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    print("  server:       " + " ".join(command), flush=True)
    print(
        f"  HTTP mode:    parallel; max concurrency {max_concurrency}",
        flush=True,
    )
    base._append_timeline(
        timeline,
        "server_starting",
        cache=cache_name,
        cache_plan=str(cache_plan),
        port=port,
    )
    process = subprocess.Popen(command, cwd=base.ROOT)

    successful_runs = 0
    failed_runs = 0
    runtime_load_ms = 0.0
    measurements_path = run_dir / "measurements.csv"
    outputs_dir = run_dir / "outputs" / "requests"
    request_dir = run_dir / "requests"
    request_events_dir = run_dir / "events" / "requests"
    outputs_dir.mkdir(parents=True, exist_ok=True)
    request_dir.mkdir(parents=True, exist_ok=True)
    request_events_dir.mkdir(parents=True, exist_ok=True)
    intervals: list[dict[str, Any]] = []

    fields = [
        "run_index",
        "request_id",
        "start_uuid",
        "repetition",
        "measured",
        "scheduled_at_ms",
        "actual_at_ms",
        "schedule_delay_ms",
        "status",
        "http_status",
        "duration_ms",
        "server_bfs_ms",
        "vertex_count",
        "predecessor_count",
        "response_bytes",
        "started_unix_ns",
        "finished_unix_ns",
        "request_json",
        "output_json",
        "events_jsonl",
        "error",
    ]

    timeline_lock = threading.Lock()

    def append_timeline(event: str, **event_fields: Any) -> None:
        with timeline_lock:
            base._append_timeline(timeline, event, **event_fields)

    try:
        runtime_load_ms = base._wait_for_server(process, host, port, startup_timeout)
        print(f"  HTTP ready:   {runtime_load_ms:.3f} ms", flush=True)
        append_timeline(
            "server_ready",
            cache=cache_name,
            runtime_load_ms=runtime_load_ms,
            port=port,
        )
        client_host = "127.0.0.1" if host in {"0.0.0.0", "::"} else host
        server_ready_perf = time.perf_counter()

        def execute_request(
            run_index: int,
            request_spec: base.HttpRequestSpec | ParallelHttpRequestSpec,
        ) -> dict[str, Any]:
            if request_spec.at_ms is not None:
                target = server_ready_perf + request_spec.at_ms / 1000.0
                delay = target - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)

            actual_at_ms = (time.perf_counter() - server_ready_perf) * 1000.0
            scheduled_at_ms = request_spec.at_ms
            schedule_delay_ms = (
                max(0.0, actual_at_ms - scheduled_at_ms)
                if scheduled_at_ms is not None
                else 0.0
            )
            endpoint = (
                f"http://{client_host}:{port}/api/v1/graphs/"
                f"{urllib.parse.quote(request_spec.start_uuid, safe='')}/bfs"
            )
            output_path = outputs_dir / f"{request_spec.request_id}.json"
            request_path = request_dir / f"{request_spec.request_id}.json"
            request_path.write_text(
                json.dumps(
                    {
                        "id": request_spec.request_id,
                        "scheduledAtMs": scheduled_at_ms,
                        "actualAtMs": actual_at_ms,
                        "startUuid": request_spec.start_uuid,
                        "measure": request_spec.measure,
                        "mode": "parallel",
                        "maxConcurrency": max_concurrency,
                        "endpoint": endpoint,
                        "payload": request_spec.payload,
                    },
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )

            started_ns = time.time_ns()
            append_timeline(
                "bfs_start",
                cache=cache_name,
                run_index=run_index,
                request_id=request_spec.request_id,
                start_uuid=request_spec.start_uuid,
                repetition=request_spec.repetition,
                measured=request_spec.measure,
                scheduled_at_ms=scheduled_at_ms,
                actual_at_ms=actual_at_ms,
                schedule_delay_ms=schedule_delay_ms,
            )
            transport_failure = False
            try:
                status_code, raw, body, duration_ms = base._post_bfs(
                    endpoint, request_spec.payload, request_timeout
                )
                finished_ns = time.time_ns()
                success = 200 <= status_code < 300
                status = "success" if success else "failed"
                server_bfs_ms = float(body.get("bfsDurationNs", 0)) / 1_000_000.0
                vertex_count = int(body.get("vertexCount", 0))
                predecessor_count = int(body.get("predecessorCount", 0))
                error = "" if success else str(
                    body.get("error", body.get("message", body))
                )
                output_path.write_bytes(raw)
            except Exception as error_value:  # noqa: BLE001
                transport_failure = True
                finished_ns = time.time_ns()
                duration_ms = (finished_ns - started_ns) / 1_000_000.0
                status_code = 0
                status = "failed"
                server_bfs_ms = 0.0
                vertex_count = 0
                predecessor_count = 0
                raw = b""
                error = str(error_value)
                output_path.write_text(
                    json.dumps({"error": error}, indent=2) + "\n",
                    encoding="utf-8",
                )

            append_timeline(
                "bfs_finish",
                cache=cache_name,
                run_index=run_index,
                request_id=request_spec.request_id,
                start_uuid=request_spec.start_uuid,
                repetition=request_spec.repetition,
                status=status,
                duration_ms=duration_ms,
                server_bfs_ms=server_bfs_ms,
            )
            return {
                "run_index": run_index,
                "request": request_spec,
                "transport_failure": transport_failure,
                "interval": {
                    "request_id": request_spec.request_id,
                    "run_index": run_index,
                    "start_uuid": request_spec.start_uuid,
                    "measured": request_spec.measure,
                    "started_unix_ns": started_ns,
                    "finished_unix_ns": finished_ns,
                },
                "row": {
                    "run_index": run_index,
                    "request_id": request_spec.request_id,
                    "start_uuid": request_spec.start_uuid,
                    "repetition": (
                        "" if request_spec.repetition is None else request_spec.repetition
                    ),
                    "measured": request_spec.measure,
                    "scheduled_at_ms": (
                        "" if scheduled_at_ms is None else scheduled_at_ms
                    ),
                    "actual_at_ms": actual_at_ms,
                    "schedule_delay_ms": schedule_delay_ms,
                    "status": status,
                    "http_status": status_code,
                    "duration_ms": duration_ms,
                    "server_bfs_ms": server_bfs_ms,
                    "vertex_count": vertex_count,
                    "predecessor_count": predecessor_count,
                    "response_bytes": len(raw),
                    "started_unix_ns": started_ns,
                    "finished_unix_ns": finished_ns,
                    "request_json": str(request_path.resolve()),
                    "output_json": str(output_path.resolve()),
                    # Empty by design: overlapping C++ events cannot be assigned
                    # to a request from timestamps alone.
                    "events_jsonl": "",
                    "error": error,
                },
            }

        completed_results: list[dict[str, Any]] = []
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=max_concurrency,
            thread_name_prefix="polygraph-http-bfs",
        ) as executor:
            futures = [
                executor.submit(execute_request, run_index, request_spec)
                for run_index, request_spec in enumerate(requests, start=1)
            ]
            for future in concurrent.futures.as_completed(futures):
                completed_results.append(future.result())

        with measurements_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields, delimiter=";")
            writer.writeheader()
            for result in sorted(
                completed_results, key=lambda item: int(item["run_index"])
            ):
                request_spec = result["request"]
                intervals.append(result["interval"])
                writer.writerow(result["row"])
                if result["row"]["status"] == "success":
                    successful_runs += 1
                else:
                    failed_runs += 1
                print(
                    f"    [{result['run_index']}/{len(requests)}] "
                    f"{request_spec.request_id} start={request_spec.start_uuid}: "
                    f"{result['row']['status']}; "
                    f"HTTP {float(result['row']['duration_ms']):.3f} ms; "
                    f"server BFS {float(result['row']['server_bfs_ms']):.3f} ms; "
                    f"schedule delay {float(result['row']['schedule_delay_ms']):.3f} ms",
                    flush=True,
                )
    finally:
        append_timeline("server_stopping", cache=cache_name, port=port)
        base._stop_server(process)
        append_timeline(
            "server_stopped",
            cache=cache_name,
            port=port,
            returncode=process.returncode,
        )

    event_files = _split_server_events_parallel_aware(
        run_dir / "events" / "server.events.jsonl",
        request_events_dir,
        intervals,
    )
    summary = {
        "transport": "http",
        "runtimeLoadDurationMs": runtime_load_ms,
        "successfulRunCount": successful_runs,
        "failedRunCount": failed_runs,
        "httpHost": host,
        "httpPort": port,
        "requestCount": len(intervals),
        "measuredRequestCount": sum(1 for request in requests if request.measure),
        "requestMode": "parallel",
        "maxConcurrency": max_concurrency,
        "perRequestEvents": event_files,
    }
    (run_dir / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    return base.HttpBenchmarkResult(
        returncode=0 if failed_runs == 0 else 1,
        runtime_load_ms=runtime_load_ms,
        successful_runs=successful_runs,
        failed_runs=failed_runs,
    )


def main() -> int:
    # base.main resolves these module globals at runtime, so the existing CLI,
    # cache planning, Redis loading, comparison files, etc. remain unchanged.
    base._load_http_request_plan = _load_http_request_plan
    base._run_http_benchmark = _run_http_benchmark
    return base.main()


if __name__ == "__main__":
    raise SystemExit(main())
