#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from statistics import mean
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.cache.load_redis import load as load_redis_cache  # noqa: E402
from tools.graphio.cache_planner import load_cache_config, plan_graph_caches  # noqa: E402
from tools.graphio.domain import load_domain_manifest  # noqa: E402


@dataclass(frozen=True)
class HttpBenchmarkResult:
    returncode: int
    runtime_load_ms: float
    successful_runs: int
    failed_runs: int


@dataclass(frozen=True)
class HttpRequestSpec:
    request_id: str
    start_uuid: str
    payload: dict[str, Any]
    at_ms: float | None = None
    repetition: int | None = None
    measure: bool = True
    mode: str = "serial"
    max_concurrency: int | None = None


def _safe_name(path: Path, used: set[str]) -> str:
    base = re.sub(r"[^A-Za-z0-9._-]+", "-", path.stem).strip("-.") or "cache"
    name = base
    suffix = 2
    while name in used:
        name = f"{base}-{suffix}"
        suffix += 1
    used.add(name)
    return name



def _existing_plan_name(path: Path, used: set[str]) -> str:
    candidate = path.parent.name if path.name == "cache-plan.json" else path.stem
    return _safe_name(Path(candidate), used)


def _resolve_plan_path(plan_path: Path, value: str | Path) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = plan_path.parent / path
    return path.resolve()


def _validate_binary_artifact(plan_path: Path, name: str, artifact: dict[str, Any]) -> None:
    manifest_value = artifact.get("manifest")
    if not manifest_value:
        raise ValueError(f"Existing cache plan is missing {name} artifact manifest: {plan_path}")
    manifest_path = _resolve_plan_path(plan_path, str(manifest_value))
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Existing {name} cache manifest is missing: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for field in ("index", "neighbors"):
        value = manifest.get(field)
        if not value:
            raise ValueError(f"{name} manifest is missing '{field}': {manifest_path}")
        artifact_path = (manifest_path.parent / str(value)).resolve()
        if not artifact_path.is_file():
            raise FileNotFoundError(f"Existing {name} cache artifact is missing: {artifact_path}")



def _validate_gpu_artifacts(plan_path: Path, artifacts: dict[str, Any]) -> None:
    gpu = artifacts.get("gpu")
    if not isinstance(gpu, dict):
        return
    for device_id, artifact in gpu.items():
        if not isinstance(artifact, dict) or not artifact.get("manifest"):
            raise ValueError(f"Existing GPU:{device_id} cache artifact has no manifest: {plan_path}")
        manifest_path = _resolve_plan_path(plan_path, str(artifact["manifest"]))
        if not manifest_path.is_file():
            raise FileNotFoundError(f"Existing GPU:{device_id} projection manifest is missing: {manifest_path}")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("format") != "polygraph.gpu-projection.dense-edgelist":
            raise ValueError(f"Unsupported GPU projection artifact: {manifest_path}")
        for field in ("globalIds", "sources", "destinations"):
            value = manifest.get(field)
            if not value:
                raise ValueError(f"GPU:{device_id} manifest is missing '{field}': {manifest_path}")
            artifact_path = (manifest_path.parent / str(value)).resolve()
            if not artifact_path.is_file():
                raise FileNotFoundError(f"Existing GPU:{device_id} projection artifact is missing: {artifact_path}")


def _load_existing_cache_plan(plan_path: str | Path, expected_graph: Path) -> dict[str, Any]:
    path = Path(plan_path).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(path)
    plan = json.loads(path.read_text(encoding="utf-8"))
    if plan.get("format") != "polygraph.cache-plan":
        raise ValueError(f"Not a PolyGraph cache plan: {path}")

    graph_value = plan.get("graph")
    if not graph_value:
        raise ValueError(f"Existing cache plan does not identify its graph: {path}")
    graph_path = _resolve_plan_path(path, str(graph_value))
    if graph_path != expected_graph.resolve():
        raise ValueError(
            f"Cache plan graph mismatch: {path} references {graph_path}, "
            f"but the selected domain uses {expected_graph.resolve()}"
        )

    artifacts = plan.get("artifacts", {})
    residency = artifacts.get("residency")
    if not isinstance(residency, dict) or not residency.get("path"):
        raise ValueError(f"Existing cache plan has no residency artifact: {path}")
    residency_path = _resolve_plan_path(path, str(residency["path"]))
    if not residency_path.is_file():
        raise FileNotFoundError(f"Existing residency artifact is missing: {residency_path}")

    for name in ("mmap", "redis"):
        artifact = artifacts.get(name)
        if isinstance(artifact, dict):
            _validate_binary_artifact(path, name, artifact)
    _validate_gpu_artifacts(path, artifacts)

    return {**plan, "plan": str(path)}


def _plan_uses_redis(plan: dict[str, Any]) -> bool:
    if not isinstance(plan.get("artifacts", {}).get("redis"), dict):
        return False
    redis_config = plan.get("redis", {})
    if isinstance(redis_config, dict) and redis_config.get("enabled", False):
        return True
    return any(
        str(backend.get("id")) == "Redis" and backend.get("enabled", True)
        for backend in plan.get("runtimeBackends", [])
        if isinstance(backend, dict)
    )

def _safe_component(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9._-]+", "_", value)[:96]
    if not result or result in {".", ".."}:
        raise ValueError(f"Could not make a safe output path component from {value!r}")
    return result


def _find_benchmark_binary(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(path)
        return path
    candidates = [
        ROOT / "build/gpu-bfs/bin/polygraph_bfs_benchmark",
        ROOT / "build/bin/polygraph_bfs_benchmark",
        ROOT / "build/release/bin/polygraph_bfs_benchmark",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        "Could not find polygraph_bfs_benchmark; pass --benchmark-binary explicitly"
    )


def _find_http_server_binary(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(path)
        return path
    candidates = [
        ROOT / "build/gpu-bfs/bin/polygraph_http_server",
        ROOT / "build/bin/polygraph_http_server",
        ROOT / "build/release/bin/polygraph_http_server",
        ROOT / "build/http-server/bin/polygraph_http_server"
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        "Could not find polygraph_http_server; pass --http-server-binary explicitly"
    )


def _read_measurements(path: Path) -> list[float]:
    if not path.is_file():
        return []
    values: list[float] = []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle, delimiter=";")
        for row in reader:
            measured = str(row.get("measured", "true")).strip().lower() not in {"false", "0", "no"}
            if row.get("status") == "success" and measured:
                values.append(float(row["duration_ms"]))
    return values


def _write_comparison(output_dir: Path, rows: list[dict]) -> None:
    (output_dir / "comparison.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    fields = [
        "cache_name",
        "cache_config",
        "cache_plan",
        "transport",
        "status",
        "cache_plan_build_ms",
        "planning_db_reused",
        "residency_entries",
        "gpu_vertices",
        "gpu_projection_vertices",
        "gpu_adjacency_pairs",
        "gpu_artifact_bytes",
        "gpu_estimated_peak_device_bytes",
        "redis_plan_vertices",
        "redis_unique_vertices",
        "redis_shared_hot_vertices",
        "redis_load_ms",
        "runtime_load_ms",
        "successful_runs",
        "failed_runs",
        "mean_bfs_ms",
        "mean_server_bfs_ms",
        "min_bfs_ms",
        "max_bfs_ms",
        "redis_loaded_vertices",
    ]
    with (output_dir / "comparison.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})

    lines = [
        "# Cache benchmark comparison",
        "",
        "| Cache | Transport | Status | Plan build (ms) | Runtime load (ms) | HTTP/BFS mean (ms) | Server BFS mean (ms) | Redis unique/shared |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        mean_value = row.get("mean_bfs_ms")
        server_mean = row.get("mean_server_bfs_ms")
        lines.append(
            "| {cache} | {transport} | {status} | {plan:.3f} | {load:.3f} | {mean} | {server_mean} | {unique}/{shared} |".format(
                cache=row.get("cache_name", ""),
                transport=row.get("transport", ""),
                status=row.get("status", ""),
                plan=float(row.get("cache_plan_build_ms") or 0.0),
                load=float(row.get("runtime_load_ms") or 0.0),
                mean="n/a" if mean_value is None else f"{float(mean_value):.3f}",
                server_mean="n/a" if server_mean is None else f"{float(server_mean):.3f}",
                unique=row.get("redis_unique_vertices", 0),
                shared=row.get("redis_shared_hot_vertices", 0),
            )
        )
    (output_dir / "comparison.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _read_start_uuids(single: str | None, file_path: str | None, fallback: str) -> list[str]:
    if single:
        return [single]
    if not file_path:
        return [fallback]
    path = Path(file_path).expanduser().resolve()
    values: list[str] = []
    seen: set[str] = set()
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        value = raw_line.split("#", 1)[0].strip()
        if not value:
            continue
        for item in value.split(","):
            item = item.strip()
            if item and item not in seen:
                seen.add(item)
                values.append(item)
    if not values:
        raise ValueError(f"Start UUID file is empty: {path}")
    return values


def _http_bfs_payload(
    extra: list[str],
    *,
    max_depth: int,
    direction: str,
    thread_count: int | None,
    neighbor_batch_size: int | None,
    response_mode: str,
) -> dict[str, Any]:
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--max-depth", type=int)
    parser.add_argument("--direction", choices=["directed", "undirected", "both"])
    parser.add_argument("--allowed-edge-types")
    parser.add_argument("--allowed-target-node-types")
    parser.add_argument("--allow-native-pushdown", action="store_true", default=None)
    parser.add_argument("--no-partial-acceleration", action="store_true", default=None)
    parser.add_argument("--no-predecessors", action="store_true", default=None)
    parser.add_argument("--neighbor-batch-size", type=int)
    parser.add_argument("--neo4j-max-concurrent-requests", type=int)
    parser.add_argument("--arango-max-concurrent-requests", type=int)
    parser.add_argument("--arango-request-strategy", choices=["chunked", "single", "auto"])
    parser.add_argument("--arango-single-request-max-frontier", type=int)
    parser.add_argument("--streaming-candidate-batch-size", type=int)
    parser.add_argument("--redis-pipeline-batch-size", type=int)
    parser.add_argument("--backend-request-retries", type=int)
    parser.add_argument("--backend-retry-delay-ms", type=int)
    parser.add_argument("--thread-count", type=int)
    parsed, unknown = parser.parse_known_args(extra)
    if unknown:
        raise ValueError(
            "These arguments cannot be represented by the HTTP BFS endpoint: " + " ".join(unknown)
        )

    effective_max_depth = max_depth if parsed.max_depth is None else parsed.max_depth
    effective_direction = direction if parsed.direction is None else parsed.direction
    effective_thread_count = thread_count if parsed.thread_count is None else parsed.thread_count
    effective_neighbor_batch = (
        neighbor_batch_size if parsed.neighbor_batch_size is None else parsed.neighbor_batch_size
    )
    if effective_max_depth < -1:
        raise ValueError("--max-depth must be -1 or greater")

    positive = {
        "--thread-count": effective_thread_count,
        "--neighbor-batch-size": effective_neighbor_batch,
        "--neo4j-max-concurrent-requests": parsed.neo4j_max_concurrent_requests,
        "--arango-max-concurrent-requests": parsed.arango_max_concurrent_requests,
        "--arango-single-request-max-frontier": parsed.arango_single_request_max_frontier,
        "--streaming-candidate-batch-size": parsed.streaming_candidate_batch_size,
        "--redis-pipeline-batch-size": parsed.redis_pipeline_batch_size,
    }
    for name, value in positive.items():
        if value is not None and value <= 0:
            raise ValueError(f"{name} must be greater than zero")
    if parsed.backend_request_retries is not None and parsed.backend_request_retries < 0:
        raise ValueError("--backend-request-retries must be zero or greater")
    if parsed.backend_retry_delay_ms is not None and parsed.backend_retry_delay_ms < 0:
        raise ValueError("--backend-retry-delay-ms must be zero or greater")

    payload: dict[str, Any] = {
        "maxDepth": effective_max_depth,
        "direction": effective_direction,
        "responseMode": response_mode,
    }
    if parsed.allowed_edge_types:
        payload["allowedEdgeTypes"] = [item.strip() for item in parsed.allowed_edge_types.split(",") if item.strip()]
    if parsed.allowed_target_node_types:
        payload["allowedTargetNodeTypes"] = [
            item.strip() for item in parsed.allowed_target_node_types.split(",") if item.strip()
        ]
    if parsed.allow_native_pushdown:
        payload["allowNativePushdown"] = True
    if parsed.no_partial_acceleration:
        payload["allowPartialAcceleration"] = False
    if parsed.no_predecessors:
        payload["returnPredecessors"] = False
    if effective_neighbor_batch is not None:
        payload["neighborBatchSize"] = effective_neighbor_batch
    if parsed.neo4j_max_concurrent_requests is not None:
        payload["neo4jMaxConcurrentRequests"] = parsed.neo4j_max_concurrent_requests
    if parsed.arango_max_concurrent_requests is not None:
        payload["arangoMaxConcurrentRequests"] = parsed.arango_max_concurrent_requests
    if parsed.arango_request_strategy is not None:
        payload["arangoRequestStrategy"] = parsed.arango_request_strategy
    if parsed.arango_single_request_max_frontier is not None:
        payload["arangoSingleRequestMaxFrontier"] = parsed.arango_single_request_max_frontier
    if parsed.streaming_candidate_batch_size is not None:
        payload["streamingCandidateBatchSize"] = parsed.streaming_candidate_batch_size
    if parsed.redis_pipeline_batch_size is not None:
        payload["redisPipelineBatchSize"] = parsed.redis_pipeline_batch_size
    if parsed.backend_request_retries is not None:
        payload["backendRequestRetries"] = parsed.backend_request_retries
    if parsed.backend_retry_delay_ms is not None:
        payload["backendRetryDelayMs"] = parsed.backend_retry_delay_ms
    if effective_thread_count is not None:
        payload["threadCount"] = effective_thread_count
    return payload


def _load_http_request_plan(
    path: str | Path,
    *,
    domain_start_uuid: str,
    base_payload: dict[str, Any],
) -> list[HttpRequestSpec]:
    source = Path(path).expanduser().resolve()
    raw = json.loads(source.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("HTTP request plan root must be a JSON object")
    mode = str(raw.get("mode", "serial")).strip().lower()
    if mode not in {"serial", "parallel"}:
        raise ValueError("HTTP request plan mode must be 'serial' or 'parallel'")

    max_concurrency_raw = raw.get("maxConcurrency")
    max_concurrency: int | None = None
    if max_concurrency_raw is not None:
        if isinstance(max_concurrency_raw, bool):
            raise ValueError("HTTP request plan maxConcurrency must be an integer")
        max_concurrency = int(max_concurrency_raw)
        if max_concurrency <= 0:
            raise ValueError("HTTP request plan maxConcurrency must be greater than zero")
        if mode != "parallel":
            raise ValueError(
                "HTTP request plan maxConcurrency is only valid with mode='parallel'"
            )

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

    result: list[HttpRequestSpec] = []
    used_ids: set[str] = set()
    previous_at_ms = -1.0
    for index, item in enumerate(items, start=1):
        if not isinstance(item, dict):
            raise ValueError(f"HTTP request plan requests[{index - 1}] must be an object")
        request_id = _safe_component(str(item.get("id", f"request-{index:03d}")))
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
        payload = {**base_payload, **default_payload, **request_payload}
        result.append(
            HttpRequestSpec(
                request_id=request_id,
                start_uuid=start_uuid,
                payload=payload,
                at_ms=at_ms,
                measure=measure,
                mode=mode,
                max_concurrency=max_concurrency,
            )
        )
    return result


def _default_http_requests(
    start_uuids: list[str],
    repetitions: int,
    payload: dict[str, Any],
) -> list[HttpRequestSpec]:
    result: list[HttpRequestSpec] = []
    run_index = 0
    for source in start_uuids:
        source_component = _safe_component(source)
        for repetition in range(1, repetitions + 1):
            run_index += 1
            result.append(
                HttpRequestSpec(
                    request_id=f"{run_index:04d}-{source_component}-r{repetition:03d}",
                    start_uuid=source,
                    payload=dict(payload),
                    at_ms=None,
                    repetition=repetition,
                )
            )
    return result


def _split_server_events_by_request(
    server_events: Path,
    output_dir: Path,
    intervals: list[dict[str, Any]],
) -> dict[str, str]:
    output_dir.mkdir(parents=True, exist_ok=True)
    result = {
        str(interval["request_id"]): str((output_dir / f"{interval['request_id']}.events.jsonl").resolve())
        for interval in intervals
    }
    for destination in result.values():
        Path(destination).write_text("", encoding="utf-8")
    if not server_events.is_file() or not intervals:
        return result

    ordered = sorted(intervals, key=lambda item: int(item["started_unix_ns"]))
    interval_index = 0
    current_handle = None
    try:
        with server_events.open("r", encoding="utf-8") as source:
            for line in source:
                try:
                    event = json.loads(line)
                    timestamp = int(event.get("timestamp_unix_ns", -1))
                except (ValueError, TypeError, json.JSONDecodeError):
                    continue
                while (
                    interval_index < len(ordered)
                    and timestamp > int(ordered[interval_index]["finished_unix_ns"])
                ):
                    if current_handle is not None:
                        current_handle.close()
                        current_handle = None
                    interval_index += 1
                if interval_index >= len(ordered):
                    break
                interval = ordered[interval_index]
                if timestamp < int(interval["started_unix_ns"]):
                    continue
                if current_handle is None:
                    current_handle = Path(result[str(interval["request_id"])]).open(
                        "a", encoding="utf-8"
                    )
                current_handle.write(line)
    finally:
        if current_handle is not None:
            current_handle.close()

    (output_dir / "index.json").write_text(
        json.dumps(
            [
                {
                    **interval,
                    "events_jsonl": result[str(interval["request_id"])],
                }
                for interval in intervals
            ],
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return result


def _redirect_observability(source: Path, run_dir: Path) -> Path:
    config = json.loads(source.read_text(encoding="utf-8"))
    events_path = (run_dir / "events" / "server.events.jsonl").resolve()
    log_path = (run_dir / "logs" / "server.log").resolve()
    events_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)

    def redirect_file_sinks(section: dict[str, Any], destination: Path, *, event: bool) -> None:
        sinks = section.setdefault("sinks", [])
        found = False
        for sink in sinks:
            if sink.get("type") not in {"file", "basic_file", "rotating_file"}:
                continue
            if found:
                sink["enabled"] = False
                continue
            found = True
            sink["enabled"] = True
            sink["type"] = "basic_file"
            sink["path"] = str(destination)
            sink["truncate"] = True
            sink.pop("max_size_mb", None)
            sink.pop("max_files", None)
            sink.pop("rotate_on_open", None)
            if event:
                sink["format"] = "json"
                sink["color"] = False
        if not found:
            sink: dict[str, Any] = {
                "name": "benchmark-events" if event else "benchmark-log",
                "enabled": True,
                "type": "basic_file",
                "path": str(destination),
                "truncate": True,
            }
            if event:
                sink.update({"format": "json", "color": False})
            sinks.append(sink)

    redirect_file_sinks(config.setdefault("events", {}), events_path, event=True)
    redirect_file_sinks(config.setdefault("logging", {}), log_path, event=False)
    destination = run_dir / "server" / "observability.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    return destination.resolve()


def _http_server_config(
    base_config: Path,
    *,
    domain: Path,
    cache_plan: Path,
    observability: Path,
    host: str,
    port: int,
    output: Path,
) -> Path:
    config = json.loads(base_config.read_text(encoding="utf-8"))
    config["listeners"] = [{"address": host, "port": port, "https": False}]
    plugins = config.setdefault("plugins", [])
    plugin = next(
        (entry for entry in plugins if entry.get("name") == "polygraph::services::ServicesPlugin"),
        None,
    )
    if plugin is None:
        plugin = {"name": "polygraph::services::ServicesPlugin", "dependencies": [], "config": {}}
        plugins.append(plugin)
    plugin_config = plugin.setdefault("config", {})
    plugin_config["domain"] = str(domain.resolve())
    plugin_config["cache_plan"] = str(cache_plan.resolve())
    plugin_config["observability_config"] = str(observability.resolve())
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    return output.resolve()


def _health_url(host: str, port: int) -> str:
    client_host = "127.0.0.1" if host in {"0.0.0.0", "::"} else host
    return f"http://{client_host}:{port}/healthz"


def _wait_for_server(process: subprocess.Popen, host: str, port: int, timeout_seconds: float) -> float:
    started = time.perf_counter()
    deadline = None if timeout_seconds <= 0 else started + timeout_seconds
    last_error: Exception | None = None
    url = _health_url(host, port)
    while deadline is None or time.perf_counter() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"polygraph_http_server exited during startup with code {process.returncode}")
        try:
            # Keep each health probe bounded even when overall startup has no timeout.
            with urllib.request.urlopen(url, timeout=1.0) as response:
                if response.status == 200:
                    return (time.perf_counter() - started) * 1000.0
        except (OSError, urllib.error.URLError) as error:
            last_error = error
        time.sleep(0.2)
    raise TimeoutError(f"Timed out waiting for {url}: {last_error}")


def _stop_server(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=20.0)
        return
    except subprocess.TimeoutExpired:
        process.terminate()
    try:
        process.wait(timeout=10.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def _append_timeline(path: Path, event: str, **fields: Any) -> None:
    record = {"timestamp_unix_ns": time.time_ns(), "event": event, **fields}
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, separators=(",", ":")) + "\n")


def _post_bfs(url: str, payload: dict[str, Any], timeout_seconds: float) -> tuple[int, bytes, dict[str, Any], float]:
    request = urllib.request.Request(
        url,
        data=json.dumps(payload, separators=(",", ":")).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        method="POST",
    )
    started = time.perf_counter_ns()
    try:
        # urllib uses blocking I/O here.  A non-positive timeout deliberately means
        # "no client deadline": the benchmark will not start repetition N+1 until
        # the HTTP response for repetition N has completed (or the connection fails).
        if timeout_seconds <= 0:
            response_context = urllib.request.urlopen(request)
        else:
            response_context = urllib.request.urlopen(request, timeout=timeout_seconds)
        with response_context as response:
            raw = response.read()
            status = response.status
    except urllib.error.HTTPError as error:
        raw = error.read()
        status = error.code
    duration_ms = (time.perf_counter_ns() - started) / 1_000_000.0
    try:
        body = json.loads(raw.decode("utf-8")) if raw else {}
    except json.JSONDecodeError:
        body = {"raw": raw.decode("utf-8", errors="replace")}
    return status, raw, body, duration_ms


def _run_http_benchmark(
    *,
    server_binary: Path,
    base_server_config: Path,
    domain_manifest: Path,
    observability_source: Path,
    cache_plan: Path,
    run_dir: Path,
    requests: list[HttpRequestSpec],
    cooldown_ms: int,
    host: str,
    port: int,
    startup_timeout: float,
    request_timeout: float,
    cold_prime_timeout: float,
    timeline: Path,
    cache_name: str,
) -> HttpBenchmarkResult:
    if requests and requests[0].mode == "parallel":
        from tools.measurement.benchmark_cache_parallel import run_http_benchmark_parallel

        return run_http_benchmark_parallel(
            server_binary=server_binary,
            base_server_config=base_server_config,
            domain_manifest=domain_manifest,
            observability_source=observability_source,
            cache_plan=cache_plan,
            run_dir=run_dir,
            requests=requests,
            host=host,
            port=port,
            startup_timeout=startup_timeout,
            request_timeout=request_timeout,
            timeline=timeline,
            cache_name=cache_name,
        )

    if not 1 <= port <= 65535:
        raise ValueError("--http-port must be between 1 and 65535")
    run_dir.mkdir(parents=True, exist_ok=True)
    observability = _redirect_observability(observability_source, run_dir)
    server_config = _http_server_config(
        base_server_config,
        domain=domain_manifest,
        cache_plan=cache_plan,
        observability=observability,
        host=host,
        port=port,
        output=run_dir / "server" / "drogon.json",
    )
    command = [str(server_binary), "--server-config", str(server_config)]
    (run_dir / "server" / "command.txt").write_text(" ".join(command) + "\n", encoding="utf-8")
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
                }
                for request in requests
            ],
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    print("  server:       " + " ".join(command), flush=True)
    _append_timeline(timeline, "server_starting", cache=cache_name, cache_plan=str(cache_plan), port=port)
    process = subprocess.Popen(command, cwd=ROOT)
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
    cold_prime_summary: dict[str, Any] | None = None

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
    try:
        runtime_load_ms = _wait_for_server(process, host, port, startup_timeout)
        print(f"  HTTP ready:   {runtime_load_ms:.3f} ms", flush=True)
        _append_timeline(timeline, "server_ready", cache=cache_name, runtime_load_ms=runtime_load_ms, port=port)
        client_host = "127.0.0.1" if host in {"0.0.0.0", "::"} else host
        server_ready_perf = time.perf_counter()

        # A cold ArangoDB restart can make the first BFS orders of magnitude slower
        # than steady state. When requested, issue one completely unmeasured prime
        # using the first request's root/payload, give it an independent generous
        # timeout, persist the result, and only then begin the ordinary warmup plan.
        # This keeps the normal per-request timeout strict without letting the cold
        # first request abort the entire configuration.
        if cold_prime_timeout > 0 and requests:
            prime = requests[0]
            prime_endpoint = (
                f"http://{client_host}:{port}/api/v1/graphs/"
                f"{urllib.parse.quote(prime.start_uuid, safe='')}/bfs"
            )
            prime_started_ns = time.time_ns()
            _append_timeline(
                timeline,
                "cold_prime_start",
                cache=cache_name,
                start_uuid=prime.start_uuid,
                timeout_seconds=cold_prime_timeout,
            )
            print(
                f"    [cold-prime] start={prime.start_uuid}; timeout={cold_prime_timeout:g}s; unmeasured",
                flush=True,
            )
            prime_record: dict[str, Any] = {
                "startUuid": prime.start_uuid,
                "timeoutSeconds": cold_prime_timeout,
                "measured": False,
                "payload": prime.payload,
            }
            try:
                prime_status, prime_raw, prime_body, prime_duration_ms = _post_bfs(
                    prime_endpoint, prime.payload, cold_prime_timeout
                )
                prime_finished_ns = time.time_ns()
                prime_success = 200 <= prime_status < 300
                prime_server_bfs_ms = float(prime_body.get("bfsDurationNs", 0)) / 1_000_000.0
                prime_record.update(
                    {
                        "status": "success" if prime_success else "failed",
                        "httpStatus": prime_status,
                        "durationMs": prime_duration_ms,
                        "serverBfsMs": prime_server_bfs_ms,
                        "startedUnixNs": prime_started_ns,
                        "finishedUnixNs": prime_finished_ns,
                        "response": prime_body,
                    }
                )
                print(
                    f"    [cold-prime] {'success' if prime_success else 'failed'}; "
                    f"HTTP {prime_duration_ms:.3f} ms; server BFS {prime_server_bfs_ms:.3f} ms",
                    flush=True,
                )
                _append_timeline(
                    timeline,
                    "cold_prime_finish",
                    cache=cache_name,
                    start_uuid=prime.start_uuid,
                    status=prime_record["status"],
                    http_status=prime_status,
                    duration_ms=prime_duration_ms,
                    server_bfs_ms=prime_server_bfs_ms,
                )
                if not prime_success:
                    raise RuntimeError(
                        f"cold-prime BFS returned HTTP {prime_status}: "
                        f"{prime_body.get('error', prime_body.get('message', prime_body))}"
                    )
            except Exception as prime_error:
                prime_finished_ns = time.time_ns()
                prime_record.update(
                    {
                        "status": "failed",
                        "httpStatus": 0,
                        "durationMs": (prime_finished_ns - prime_started_ns) / 1_000_000.0,
                        "serverBfsMs": 0.0,
                        "startedUnixNs": prime_started_ns,
                        "finishedUnixNs": prime_finished_ns,
                        "error": str(prime_error),
                    }
                )
                (run_dir / "cold-prime.json").write_text(
                    json.dumps(prime_record, indent=2) + "\n", encoding="utf-8"
                )
                cold_prime_summary = prime_record
                _append_timeline(
                    timeline,
                    "cold_prime_failed",
                    cache=cache_name,
                    start_uuid=prime.start_uuid,
                    error=str(prime_error),
                )
                raise
            (run_dir / "cold-prime.json").write_text(
                json.dumps(prime_record, indent=2) + "\n", encoding="utf-8"
            )
            cold_prime_summary = prime_record
            # The normal request schedule starts after priming. In particular, atMs=0
            # should mean "immediately after the prime", not "immediately after server ready".
            server_ready_perf = time.perf_counter()

        with measurements_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields, delimiter=";")
            writer.writeheader()
            abort_cache = False
            previous_finished = False
            for run_index, request_spec in enumerate(requests, start=1):
                if request_spec.at_ms is not None:
                    target = server_ready_perf + request_spec.at_ms / 1000.0
                    delay = target - time.perf_counter()
                    if delay > 0:
                        time.sleep(delay)
                elif previous_finished and cooldown_ms > 0:
                    time.sleep(cooldown_ms / 1000.0)

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
                events_path = request_events_dir / f"{request_spec.request_id}.events.jsonl"
                request_path.write_text(
                    json.dumps(
                        {
                            "id": request_spec.request_id,
                            "scheduledAtMs": scheduled_at_ms,
                            "actualAtMs": actual_at_ms,
                            "startUuid": request_spec.start_uuid,
                            "measure": request_spec.measure,
                            "endpoint": endpoint,
                            "payload": request_spec.payload,
                        },
                        indent=2,
                    )
                    + "\n",
                    encoding="utf-8",
                )

                started_ns = time.time_ns()
                _append_timeline(
                    timeline,
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
                    status_code, raw, body, duration_ms = _post_bfs(
                        endpoint,
                        request_spec.payload,
                        request_timeout,
                    )
                    finished_ns = time.time_ns()
                    success = 200 <= status_code < 300
                    status = "success" if success else "failed"
                    if success:
                        successful_runs += 1
                    else:
                        failed_runs += 1
                    server_bfs_ms = float(body.get("bfsDurationNs", 0)) / 1_000_000.0
                    vertex_count = int(body.get("vertexCount", 0))
                    predecessor_count = int(body.get("predecessorCount", 0))
                    error = "" if success else str(body.get("error", body.get("message", body)))
                    output_path.write_bytes(raw)
                except Exception as error_value:  # noqa: BLE001 - benchmark must record transport failures
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
                    failed_runs += 1
                    output_path.write_text(json.dumps({"error": error}, indent=2) + "\n", encoding="utf-8")

                intervals.append(
                    {
                        "request_id": request_spec.request_id,
                        "run_index": run_index,
                        "start_uuid": request_spec.start_uuid,
                        "started_unix_ns": started_ns,
                        "finished_unix_ns": finished_ns,
                    }
                )
                writer.writerow(
                    {
                        "run_index": run_index,
                        "request_id": request_spec.request_id,
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
                        "events_jsonl": str(events_path.resolve()),
                        "error": error,
                    }
                )
                handle.flush()
                _append_timeline(
                    timeline,
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
                print(
                    f"    [{run_index}/{len(requests)}] {request_spec.request_id} "
                    f"start={request_spec.start_uuid}: {status}; HTTP {duration_ms:.3f} ms; "
                    f"server BFS {server_bfs_ms:.3f} ms; schedule delay {schedule_delay_ms:.3f} ms",
                    flush=True,
                )
                previous_finished = True
                if transport_failure:
                    abort_cache = True
                    _append_timeline(
                        timeline,
                        "benchmark_aborted_transport_failure",
                        cache=cache_name,
                        run_index=run_index,
                        request_id=request_spec.request_id,
                        start_uuid=request_spec.start_uuid,
                        error=error,
                    )
                    print(
                        "      transport failed; stopping this cache server before any further BFS request",
                        flush=True,
                    )
                if abort_cache:
                    break
    finally:
        _append_timeline(timeline, "server_stopping", cache=cache_name, port=port)
        _stop_server(process)
        _append_timeline(timeline, "server_stopped", cache=cache_name, port=port, returncode=process.returncode)

    event_files = _split_server_events_by_request(
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
        "measuredRequestCount": sum(1 for request in requests[:len(intervals)] if request.measure),
        "coldPrime": cold_prime_summary,
        "perRequestEvents": event_files,
    }
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return HttpBenchmarkResult(
        returncode=0 if failed_runs == 0 else 1,
        runtime_load_ms=runtime_load_ms,
        successful_runs=successful_runs,
        failed_runs=failed_runs,
    )

def _read_server_bfs_measurements(path: Path) -> list[float]:
    if not path.is_file():
        return []
    values: list[float] = []
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle, delimiter=";"):
            measured = str(row.get("measured", "true")).strip().lower() not in {"false", "0", "no"}
            if row.get("status") == "success" and measured and row.get("server_bfs_ms"):
                values.append(float(row["server_bfs_ms"]))
    return values


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark multiple cache configurations or already-generated cache plans against one immutable "
            "PolyGraph domain. Use --transport http to execute every BFS through the real Drogon endpoint."
        )
    )
    parser.add_argument("--domain", required=True, help="Prepared domain.json")
    cache_inputs = parser.add_mutually_exclusive_group(required=True)
    cache_inputs.add_argument(
        "--cache-configs",
        nargs="+",
        help="Cache configuration JSON files to generate/materialize before benchmarking.",
    )
    cache_inputs.add_argument(
        "--cache-plans",
        nargs="+",
        help="Existing cache-plan.json files to validate and reuse without regenerating cache artifacts.",
    )
    parser.add_argument("--transport", choices=["binary", "http"], default="binary")
    parser.add_argument("--benchmark-binary")
    parser.add_argument("--http-server-binary")
    parser.add_argument("--http-server-config", default=str(ROOT / "config/drogon/http_server.full.json"))
    parser.add_argument("--http-host", default="0.0.0.0")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument(
        "--http-startup-timeout",
        type=float,
        default=0.0,
        help="Seconds to wait for /healthz; 0 disables the overall startup timeout (default).",
    )
    parser.add_argument(
        "--http-request-timeout",
        type=float,
        default=0.0,
        help="Per-BFS HTTP timeout in seconds; 0 waits indefinitely and keeps repetitions strictly serial (default).",
    )
    parser.add_argument("--http-response-mode", choices=["summary", "full"], default="summary")
    parser.add_argument(
        "--cold-prime-request-timeout",
        type=float,
        default=0.0,
        help=(
            "Before the normal HTTP request plan, issue one unmeasured BFS using the first "
            "request root/payload with this independent timeout. 0 disables cold priming."
        ),
    )
    parser.add_argument(
        "--http-request-plan",
        help=(
            "JSON request schedule. Each request can define id, atMs (relative to server ready), "
            "startUuid, and a raw HTTP BFS payload override. Requests are intentionally serial."
        ),
    )
    parser.add_argument("--output-dir")
    parser.add_argument("--cache-root")
    parser.add_argument("--planning-db")
    parser.add_argument("--start-uuid")
    parser.add_argument("--start-uuids-file")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--max-depth", type=int, default=10)
    parser.add_argument("--direction", choices=["directed", "undirected", "both"], default="both")
    parser.add_argument("--cooldown-ms", type=int, default=0)
    parser.add_argument("--thread-count", type=int)
    parser.add_argument("--neighbor-batch-size", type=int)
    parser.add_argument("--skip-redis-load", action="store_true")
    parser.add_argument("--redis-no-reset", action="store_true")
    parser.add_argument("--redis-batch-size", type=int, default=10_000)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("benchmark_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if args.repetitions <= 0:
        parser.error("--repetitions must be positive")
    if args.redis_batch_size <= 0:
        parser.error("--redis-batch-size must be positive")
    if args.cooldown_ms < 0:
        parser.error("--cooldown-ms must be zero or greater")
    if args.cold_prime_request_timeout < 0:
        parser.error("--cold-prime-request-timeout must be zero or greater")
    if args.start_uuid and args.start_uuids_file:
        parser.error("--start-uuid and --start-uuids-file are mutually exclusive")
    if args.http_request_plan and args.transport != "http":
        parser.error("--http-request-plan requires --transport http")

    domain = load_domain_manifest(args.domain)
    timestamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    output_dir = (
        Path(args.output_dir).expanduser().resolve()
        if args.output_dir
        else ROOT / "measurements" / f"cache-comparison-{timestamp}"
    )
    cache_root = (
        Path(args.cache_root).expanduser().resolve()
        if args.cache_root
        else domain.paths.manifest.parent / "caches"
    ) if args.cache_configs else None
    planning_db = (
        Path(args.planning_db).expanduser().resolve()
        if args.planning_db
        else domain.paths.data.parent / "cache-plan.sqlite3"
    ) if args.cache_configs else None

    if output_dir.exists() and args.overwrite:
        shutil.rmtree(output_dir)
    elif output_dir.exists() and any(output_dir.iterdir()):
        raise ValueError(f"Output directory is not empty: {output_dir}; use --overwrite")
    output_dir.mkdir(parents=True, exist_ok=True)
    if cache_root is not None:
        cache_root.mkdir(parents=True, exist_ok=True)
    timeline = output_dir / "benchmark.timeline.jsonl"

    extra = list(args.benchmark_args)
    if extra and extra[0] == "--":
        extra.pop(0)

    benchmark_binary = _find_benchmark_binary(args.benchmark_binary) if args.transport == "binary" else None
    http_server_binary = _find_http_server_binary(args.http_server_binary) if args.transport == "http" else None
    http_server_config = Path(args.http_server_config).expanduser().resolve()
    if args.transport == "http" and not http_server_config.is_file():
        raise FileNotFoundError(http_server_config)
    start_uuids = _read_start_uuids(args.start_uuid, args.start_uuids_file, domain.start_uuid)
    http_payload = (
        _http_bfs_payload(
            extra,
            max_depth=args.max_depth,
            direction=args.direction,
            thread_count=args.thread_count,
            neighbor_batch_size=args.neighbor_batch_size,
            response_mode=args.http_response_mode,
        )
        if args.transport == "http"
        else None
    )
    http_requests: list[HttpRequestSpec] | None = None
    if args.transport == "http":
        assert http_payload is not None
        http_requests = (
            _load_http_request_plan(
                args.http_request_plan,
                domain_start_uuid=domain.start_uuid,
                base_payload=http_payload,
            )
            if args.http_request_plan
            else _default_http_requests(start_uuids, args.repetitions, http_payload)
        )

    manifest = {
        "transport": args.transport,
        "domain": str(domain.paths.manifest),
        "cacheInputMode": "configs" if args.cache_configs else "plans",
        "cacheConfigs": [str(Path(value).expanduser().resolve()) for value in (args.cache_configs or [])],
        "cachePlans": [str(Path(value).expanduser().resolve()) for value in (args.cache_plans or [])],
        "startUuids": start_uuids,
        "repetitions": args.repetitions,
        "maxDepth": args.max_depth,
        "direction": args.direction,
        "extraArguments": extra,
        "http": {
            "host": args.http_host,
            "port": args.http_port,
            "responseMode": args.http_response_mode,
            "request": http_payload,
            "requestPlan": str(Path(args.http_request_plan).expanduser().resolve()) if args.http_request_plan else None,
            "coldPrimeRequestTimeoutSeconds": args.cold_prime_request_timeout,
            "requests": [
                {
                    "id": request.request_id,
                    "atMs": request.at_ms,
                    "startUuid": request.start_uuid,
                    "payload": request.payload,
                    "measure": request.measure,
                }
                for request in (http_requests or [])
            ],
        }
        if args.transport == "http"
        else None,
    }
    (output_dir / "benchmark.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    used_names: set[str] = set()
    comparison: list[dict] = []
    raw_inputs = args.cache_configs if args.cache_configs else args.cache_plans
    assert raw_inputs is not None

    for index, raw_input in enumerate(raw_inputs, start=1):
        if args.cache_configs:
            assert cache_root is not None and planning_db is not None
            config_path: Path | None = Path(raw_input).expanduser().resolve()
            cache_name = _safe_name(config_path, used_names)
            plan_dir = cache_root / cache_name
            print(f"\n[{index}/{len(raw_inputs)}] cache configuration: {cache_name}", flush=True)
            print(f"  config:       {config_path}", flush=True)
            print(f"  plan dir:     {plan_dir}", flush=True)
            print(f"  planning DB:  {planning_db}", flush=True)
            _append_timeline(timeline, "cache_phase_start", cache=cache_name, config=str(config_path))

            config = load_cache_config(config_path)
            plan_started = time.perf_counter()
            plan = plan_graph_caches(
                domain.paths.data,
                config,
                output_dir=plan_dir,
                planning_db_path=planning_db,
            )
            cache_plan_build_ms = (time.perf_counter() - plan_started) * 1000.0
            cache_plan = Path(plan["plan"]).resolve()
            cache_source = "generated-config"
        else:
            config_path = None
            cache_plan = Path(raw_input).expanduser().resolve()
            cache_name = _existing_plan_name(cache_plan, used_names)
            run_dir = output_dir / cache_name
            print(f"\n[{index}/{len(raw_inputs)}] existing cache plan: {cache_name}", flush=True)
            print(f"  cache plan:   {cache_plan}", flush=True)
            print("  generation:   skipped (existing artifacts are reused)", flush=True)
            _append_timeline(timeline, "cache_phase_start", cache=cache_name, cache_plan=str(cache_plan), reused=True)
            plan = _load_existing_cache_plan(cache_plan, domain.paths.data)
            cache_plan_build_ms = 0.0
            cache_source = "existing-plan"

        run_dir = output_dir / cache_name
        redis_loaded_vertices = 0
        redis_load_ms = 0.0
        if _plan_uses_redis(plan) and not args.skip_redis_load:
            redis_started = time.perf_counter()
            redis_result = load_redis_cache(
                domain.paths.manifest,
                reset=not args.redis_no_reset,
                batch_size=args.redis_batch_size,
                cache_plan=cache_plan,
            )
            redis_load_ms = (time.perf_counter() - redis_started) * 1000.0
            redis_loaded_vertices = int(redis_result["vertices"])
            print(f"  Redis:        {redis_loaded_vertices:,} vertices loaded in {redis_load_ms:.3f} ms", flush=True)
        elif _plan_uses_redis(plan):
            print("  Redis:        load skipped; existing Redis contents/readiness marker will be used", flush=True)

        if args.transport == "binary":
            assert benchmark_binary is not None
            command: list[str] = [
                str(benchmark_binary),
                "--domain",
                str(domain.paths.manifest),
                "--cache-plan",
                str(cache_plan),
                "--repetitions",
                str(args.repetitions),
                "--max-depth",
                str(args.max_depth),
                "--direction",
                args.direction,
                "--cooldown-ms",
                str(args.cooldown_ms),
                "--output-dir",
                str(run_dir),
                "--overwrite",
            ]
            if args.start_uuid:
                command += ["--start-uuid", args.start_uuid]
            elif args.start_uuids_file:
                command += ["--start-uuids-file", str(Path(args.start_uuids_file).expanduser().resolve())]
            else:
                command += ["--start-uuid", domain.start_uuid]
            if args.thread_count is not None:
                command += ["--thread-count", str(args.thread_count)]
            if args.neighbor_batch_size is not None:
                command += ["--neighbor-batch-size", str(args.neighbor_batch_size)]
            command += extra
            print("  benchmark:    " + " ".join(command), flush=True)
            completed = subprocess.run(command, cwd=ROOT, check=False)
            summary_path = run_dir / "summary.json"
            summary = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.is_file() else {}
            runtime_load_ms = float(summary.get("runtimeLoadDurationMs", 0.0))
            successful_runs = int(summary.get("successfulRunCount", 0))
            failed_runs = int(summary.get("failedRunCount", 0))
        else:
            assert http_server_binary is not None and http_requests is not None
            completed = _run_http_benchmark(
                server_binary=http_server_binary,
                base_server_config=http_server_config,
                domain_manifest=domain.paths.manifest,
                observability_source=domain.paths.observability,
                cache_plan=cache_plan,
                run_dir=run_dir,
                requests=http_requests,
                cooldown_ms=args.cooldown_ms,
                host=args.http_host,
                port=args.http_port,
                startup_timeout=args.http_startup_timeout,
                request_timeout=args.http_request_timeout,
                cold_prime_timeout=args.cold_prime_request_timeout,
                timeline=timeline,
                cache_name=cache_name,
            )
            runtime_load_ms = completed.runtime_load_ms
            successful_runs = completed.successful_runs
            failed_runs = completed.failed_runs

        durations = _read_measurements(run_dir / "measurements.csv")
        server_bfs_durations = _read_server_bfs_measurements(run_dir / "measurements.csv")
        redis_region = next((region for region in plan.get("regions", []) if region.get("location") == "Redis"), {})
        gpu_vertices = sum(
            int(region.get("vertices", 0))
            for region in plan.get("regions", [])
            if str(region.get("location", "")).startswith("GPU:")
        )
        gpu_artifacts = plan.get("artifacts", {}).get("gpu", {})
        if not isinstance(gpu_artifacts, dict):
            gpu_artifacts = {}
        gpu_projection_vertices = sum(
            int(artifact.get("vertices", 0))
            for artifact in gpu_artifacts.values()
            if isinstance(artifact, dict)
        )
        gpu_adjacency_pairs = sum(
            int(artifact.get("adjacencyPairs", 0))
            for artifact in gpu_artifacts.values()
            if isinstance(artifact, dict)
        )
        gpu_artifact_bytes = sum(
            int(artifact.get("bytes", 0))
            for artifact in gpu_artifacts.values()
            if isinstance(artifact, dict)
        )
        gpu_estimated_peak_device_bytes = max(
            (
                int(artifact.get("estimatedPeakDeviceBytes", 0))
                for artifact in gpu_artifacts.values()
                if isinstance(artifact, dict)
            ),
            default=0,
        )
        row = {
            "cache_name": cache_name,
            "cache_source": cache_source,
            "cache_config": "" if config_path is None else str(config_path),
            "cache_plan": str(cache_plan),
            "transport": args.transport,
            "status": "success" if completed.returncode == 0 else "failed",
            "cache_plan_build_ms": cache_plan_build_ms,
            "planning_db_reused": (
                True if cache_source == "existing-plan" else bool(plan.get("reusedPlanningDatabase", False))
            ),
            "residency_entries": int(plan.get("artifacts", {}).get("residency", {}).get("entries", 0)),
            "gpu_vertices": gpu_vertices,
            "gpu_projection_vertices": gpu_projection_vertices,
            "gpu_adjacency_pairs": gpu_adjacency_pairs,
            "gpu_artifact_bytes": gpu_artifact_bytes,
            "gpu_estimated_peak_device_bytes": gpu_estimated_peak_device_bytes,
            "redis_plan_vertices": int(redis_region.get("vertices", 0)),
            "redis_unique_vertices": int(redis_region.get("redisUniqueVertices", 0)),
            "redis_shared_hot_vertices": int(redis_region.get("sharedHotVertices", 0)),
            "redis_load_ms": redis_load_ms,
            "runtime_load_ms": runtime_load_ms,
            "successful_runs": successful_runs,
            "failed_runs": failed_runs,
            "mean_bfs_ms": mean(durations) if durations else None,
            "mean_server_bfs_ms": mean(server_bfs_durations) if server_bfs_durations else None,
            "min_bfs_ms": min(durations) if durations else None,
            "max_bfs_ms": max(durations) if durations else None,
            "redis_loaded_vertices": redis_loaded_vertices,
        }
        comparison.append(row)
        _write_comparison(output_dir, comparison)
        _append_timeline(timeline, "cache_phase_finish", cache=cache_name, comparison=row)
        print(
            f"  result:       {row['status']}; runtime load {row['runtime_load_ms']:.3f} ms; "
            f"mean {args.transport.upper()} BFS {row['mean_bfs_ms'] if row['mean_bfs_ms'] is not None else 'n/a'} ms",
            flush=True,
        )

    print(f"\nComparison CSV:  {output_dir / 'comparison.csv'}")
    print(f"Comparison JSON: {output_dir / 'comparison.json'}")
    print(f"Comparison MD:   {output_dir / 'comparison.md'}")
    print(f"Timeline JSONL:  {timeline}")
    return 0 if all(row["status"] == "success" for row in comparison) else 1


if __name__ == "__main__":
    raise SystemExit(main())
