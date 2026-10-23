#!/usr/bin/env python3
from __future__ import annotations

import concurrent.futures
import csv
import json
import subprocess
import threading
import time
import urllib.parse
from pathlib import Path
from typing import Any

from tools.measurement import benchmark_cache_configs as base


def _intervals_overlap(intervals: list[dict[str, Any]]) -> bool:
    if len(intervals) < 2:
        return False
    ordered = sorted(intervals, key=lambda item: int(item["started_unix_ns"]))
    latest_finish = int(ordered[0]["finished_unix_ns"])
    for interval in ordered[1:]:
        if int(interval["started_unix_ns"]) <= latest_finish:
            return True
        latest_finish = max(latest_finish, int(interval["finished_unix_ns"]))
    return False


def _split_parallel_events_by_query_id(
    server_events: Path,
    output_dir: Path,
    intervals: list[dict[str, Any]],
) -> dict[str, str]:
    """Split interleaved server events by the BFS query execution ID."""
    output_dir.mkdir(parents=True, exist_ok=True)
    ordered = sorted(intervals, key=lambda item: int(item["started_unix_ns"]))

    by_query_id = {
        int(interval["query_execution_id"]): interval
        for interval in ordered
    }
    paths = {
        query_id: output_dir / f"{interval['request_id']}.events.jsonl"
        for query_id, interval in by_query_id.items()
    }
    counts = {query_id: 0 for query_id in by_query_id}

    handles = {
        query_id: path.open("w", encoding="utf-8")
        for query_id, path in paths.items()
    }
    try:
        if server_events.is_file():
            with server_events.open("r", encoding="utf-8") as source:
                for line in source:
                    stripped = line.strip()
                    if not stripped:
                        continue
                    try:
                        event = json.loads(stripped)
                    except json.JSONDecodeError:
                        continue

                    raw_query_id = event.get("query_execution_id")
                    if raw_query_id is None:
                        continue
                    try:
                        query_id = int(raw_query_id)
                    except (TypeError, ValueError):
                        continue

                    handle = handles.get(query_id)
                    if handle is None:
                        continue
                    handle.write(stripped + "\n")
                    counts[query_id] += 1
    finally:
        for handle in handles.values():
            handle.close()

    index: list[dict[str, Any]] = []
    event_files: dict[str, str] = {}
    for interval in ordered:
        query_id = int(interval["query_execution_id"])
        path = paths[query_id].resolve()
        request_id = str(interval["request_id"])
        event_files[request_id] = str(path)
        index.append(
            {
                **interval,
                "events_jsonl": str(path),
                "event_attribution": "query-execution-id",
                "event_count": counts[query_id],
            }
        )

    (output_dir / "index.json").write_text(
        json.dumps(index, indent=2) + "\n",
        encoding="utf-8",
    )
    return event_files


def run_http_benchmark_parallel(
    *,
    server_binary: Path,
    base_server_config: Path,
    domain_manifest: Path,
    observability_source: Path,
    cache_plan: Path,
    run_dir: Path,
    requests: list[base.HttpRequestSpec],
    host: str,
    port: int,
    startup_timeout: float,
    request_timeout: float,
    timeline: Path,
    cache_name: str,
) -> base.HttpBenchmarkResult:
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
        " ".join(command) + "\n",
        encoding="utf-8",
    )
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
                    "mode": request.mode,
                    "maxConcurrency": request.max_concurrency,
                }
                for request in requests
            ],
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    print("  server:       " + " ".join(command), flush=True)
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

    fields = [
        "run_index",
        "request_id",
        "query_execution_id",
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
    results: list[dict[str, Any]] = []
    timeline_lock = threading.Lock()

    def timeline_event(event: str, **fields: Any) -> None:
        with timeline_lock:
            base._append_timeline(timeline, event, **fields)

    try:
        runtime_load_ms = base._wait_for_server(process, host, port, startup_timeout)
        print(f"  HTTP ready:   {runtime_load_ms:.3f} ms", flush=True)
        timeline_event(
            "server_ready",
            cache=cache_name,
            runtime_load_ms=runtime_load_ms,
            port=port,
        )
        client_host = "127.0.0.1" if host in {"0.0.0.0", "::"} else host
        server_ready_perf = time.perf_counter()

        def execute(run_index: int, request_spec: base.HttpRequestSpec) -> dict[str, Any]:
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
            # Reserve a high range so benchmark-provided IDs never collide with
            # ordinary auto-assigned scheduler query IDs in the same server process.
            query_execution_id = 1_000_000_000 + run_index
            request_payload = dict(request_spec.payload)
            request_payload["queryExecutionId"] = query_execution_id
            request_path.write_text(
                json.dumps(
                    {
                        "id": request_spec.request_id,
                        "queryExecutionId": query_execution_id,
                        "scheduledAtMs": scheduled_at_ms,
                        "actualAtMs": actual_at_ms,
                        "startUuid": request_spec.start_uuid,
                        "measure": request_spec.measure,
                        "mode": "parallel",
                        "maxConcurrency": request_spec.max_concurrency,
                        "endpoint": endpoint,
                        "payload": request_payload,
                    },
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )

            started_ns = time.time_ns()
            timeline_event(
                "bfs_start",
                cache=cache_name,
                run_index=run_index,
                request_id=request_spec.request_id,
                query_execution_id=query_execution_id,
                start_uuid=request_spec.start_uuid,
                repetition=request_spec.repetition,
                measured=request_spec.measure,
                scheduled_at_ms=scheduled_at_ms,
                actual_at_ms=actual_at_ms,
                schedule_delay_ms=schedule_delay_ms,
            )

            try:
                status_code, raw, body, duration_ms = base._post_bfs(
                    endpoint,
                    request_payload,
                    request_timeout,
                )
                finished_ns = time.time_ns()
                success = 200 <= status_code < 300
                status = "success" if success else "failed"
                server_bfs_ms = float(body.get("bfsDurationNs", 0)) / 1_000_000.0
                vertex_count = int(body.get("vertexCount", 0))
                predecessor_count = int(body.get("predecessorCount", 0))
                error = "" if success else str(body.get("error", body.get("message", body)))
                output_path.write_bytes(raw)
            except Exception as error_value:  # noqa: BLE001
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

            timeline_event(
                "bfs_finish",
                cache=cache_name,
                run_index=run_index,
                request_id=request_spec.request_id,
                query_execution_id=query_execution_id,
                start_uuid=request_spec.start_uuid,
                repetition=request_spec.repetition,
                status=status,
                duration_ms=duration_ms,
                server_bfs_ms=server_bfs_ms,
            )

            return {
                "run_index": run_index,
                "request": request_spec,
                "interval": {
                    "request_id": request_spec.request_id,
                    "run_index": run_index,
                    "query_execution_id": query_execution_id,
                    "start_uuid": request_spec.start_uuid,
                    "measured": request_spec.measure,
                    "started_unix_ns": started_ns,
                    "finished_unix_ns": finished_ns,
                },
                "row": {
                    "run_index": run_index,
                    "request_id": request_spec.request_id,
                    "query_execution_id": query_execution_id,
                    "start_uuid": request_spec.start_uuid,
                    "repetition": "" if request_spec.repetition is None else request_spec.repetition,
                    "measured": request_spec.measure,
                    "scheduled_at_ms": "" if scheduled_at_ms is None else scheduled_at_ms,
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
                    "events_jsonl": str((request_events_dir / f"{request_spec.request_id}.events.jsonl").resolve()),
                    "error": error,
                },
            }

        max_workers = requests[0].max_concurrency or len(requests)
        print(f"  HTTP mode:    parallel; max concurrency {max_workers}", flush=True)
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=max_workers,
            thread_name_prefix="polygraph-http-bfs",
        ) as executor:
            futures = [
                executor.submit(execute, run_index, request)
                for run_index, request in enumerate(requests, start=1)
            ]
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                results.append(result)
                row = result["row"]
                request = result["request"]
                print(
                    f"    [{result['run_index']}/{len(requests)}] {request.request_id} "
                    f"start={request.start_uuid}: {row['status']}; "
                    f"HTTP {float(row['duration_ms']):.3f} ms; "
                    f"server BFS {float(row['server_bfs_ms']):.3f} ms; "
                    f"schedule delay {float(row['schedule_delay_ms']):.3f} ms",
                    flush=True,
                )
    finally:
        timeline_event("server_stopping", cache=cache_name, port=port)
        base._stop_server(process)
        timeline_event(
            "server_stopped",
            cache=cache_name,
            port=port,
            returncode=process.returncode,
        )

    results.sort(key=lambda item: int(item["run_index"]))
    intervals = [result["interval"] for result in results]
    with measurements_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, delimiter=";")
        writer.writeheader()
        for result in results:
            writer.writerow(result["row"])
            if result["row"]["status"] == "success":
                successful_runs += 1
            else:
                failed_runs += 1

    event_files = _split_parallel_events_by_query_id(
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
        "maxConcurrency": requests[0].max_concurrency,
        "perRequestEvents": event_files,
    }
    (run_dir / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n",
        encoding="utf-8",
    )
    return base.HttpBenchmarkResult(
        returncode=0 if failed_runs == 0 else 1,
        runtime_load_ms=runtime_load_ms,
        successful_runs=successful_runs,
        failed_runs=failed_runs,
    )
