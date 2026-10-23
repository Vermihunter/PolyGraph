#!/usr/bin/env python3
"""Export Prometheus metrics for PolyGraph event-log intervals and visualize them.

The tool is intentionally independent from Grafana. Grafana remains the live
inspection UI; this script queries the Prometheus HTTP API directly so the
measurements used by a paper can be reproduced from the event logs.

Typical usage:

    python3 tools/measurement/event_metrics.py export \
      measurements/sf1-0_benchmarks/cache-comparison \
      --config config/measurement/prometheus-paper.json \
      --out-dir measurements/sf1-0_benchmarks/cache-comparison/prometheus-export

    python3 tools/measurement/event_metrics.py visualize \
      measurements/sf1-0_benchmarks/cache-comparison/prometheus-export/manifest.json

    python3 tools/measurement/event_metrics.py all \
      measurements/sf1-0_benchmarks/cache-comparison \
      --config config/measurement/prometheus-paper.json \
      --out-dir measurements/sf1-0_benchmarks/cache-comparison/prometheus-export

Range discovery is optimized for the HTTP cache benchmark layout:

    <run-type>/events/server.events.jsonl
    <run-type>/events/requests/index.json
    <run-type>/events/requests/<request-id>.events.jsonl

For request logs, events/requests/index.json is preferred because it contains the
exact HTTP request start/end timestamps. The event file itself is still stored in
the export manifest and used for optional span overlays.
"""

from __future__ import annotations

import argparse
import csv
import html
import json
import math
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping, Sequence


FORMAT_VERSION = 1
DEFAULT_TIMEOUT_SECONDS = 30.0
DEFAULT_STEP_SECONDS = 1.0


@dataclass(frozen=True)
class MetricSpec:
    metric_id: str
    label: str
    query: str
    unit: str
    panel: str
    step_seconds: float | None
    required: bool


@dataclass(frozen=True)
class Interval:
    interval_id: str
    run_type: str
    kind: str
    label: str
    event_file: Path | None
    start_unix_ns: int
    end_unix_ns: int
    request_id: str | None = None
    measured: bool | None = None

    @property
    def duration_seconds(self) -> float:
        return max(0.0, (self.end_unix_ns - self.start_unix_ns) / 1_000_000_000.0)


# ---------------------------------------------------------------------------
# Generic helpers
# ---------------------------------------------------------------------------


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=False) + "\n", encoding="utf-8")


def _safe_component(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value.strip())
    value = value.strip(".-")
    return value or "item"


def _iso_from_unix_seconds(value: float) -> str:
    return datetime.fromtimestamp(value, tz=timezone.utc).isoformat().replace("+00:00", "Z")


def _find_measurement_root(path: Path) -> Path:
    path = path.expanduser().resolve()
    if path.is_file():
        path = path.parent
    return path


def _resolve_event_file(raw: str | None, *, index_path: Path, request_id: str) -> Path:
    if raw:
        candidate = Path(raw).expanduser()
        if not candidate.is_absolute():
            candidate = (index_path.parent / candidate).resolve()
        return candidate
    return (index_path.parent / f"{request_id}.events.jsonl").resolve()


def _run_type_from_events_dir(events_dir: Path, source_root: Path) -> str:
    # canonical cache benchmark: <root>/<run-type>/events
    parent = events_dir.parent
    try:
        rel = parent.relative_to(source_root)
    except ValueError:
        rel = parent
    if rel.parts:
        return rel.parts[-1]
    return parent.name or "run"


def _event_timestamp_ns(event: Mapping[str, Any]) -> int | None:
    for key in ("timestamp_unix_ns", "timestampUnixNs", "unix_ns", "unixNs"):
        value = event.get(key)
        if value is None:
            continue
        try:
            return int(value)
        except (TypeError, ValueError):
            pass
    return None


def _event_range(path: Path) -> tuple[int, int] | None:
    first: int | None = None
    last: int | None = None
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            timestamp = _event_timestamp_ns(event)
            if timestamp is None:
                continue
            if first is None or timestamp < first:
                first = timestamp
            if last is None or timestamp > last:
                last = timestamp
    if first is None or last is None:
        return None
    return first, last


def _iter_jsonl(path: Path) -> Iterator[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                yield value


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------


def load_config(path: Path) -> dict[str, Any]:
    raw = _load_json(path.expanduser().resolve())
    if not isinstance(raw, dict):
        raise ValueError("metrics config must be a JSON object")

    prometheus = raw.get("prometheus", {})
    if not isinstance(prometheus, dict):
        raise ValueError("prometheus must be an object")
    url = str(prometheus.get("url", "http://localhost:9090")).rstrip("/")
    if not url:
        raise ValueError("prometheus.url must not be empty")
    prometheus["url"] = url
    prometheus.setdefault("timeoutSeconds", DEFAULT_TIMEOUT_SECONDS)
    prometheus.setdefault("defaultStepSeconds", DEFAULT_STEP_SECONDS)
    prometheus.setdefault("headers", {})
    raw["prometheus"] = prometheus

    ranges = raw.get("ranges", {})
    if not isinstance(ranges, dict):
        raise ValueError("ranges must be an object")
    ranges.setdefault("includeServer", True)
    ranges.setdefault("includeRequests", True)
    ranges.setdefault("includeUnindexedEventFiles", False)
    ranges.setdefault("includeWarmups", True)
    ranges.setdefault("beforeMs", 0)
    ranges.setdefault("afterMs", 0)
    raw["ranges"] = ranges

    metrics = raw.get("metrics")
    if not isinstance(metrics, list) or not metrics:
        raise ValueError("metrics must be a non-empty array")

    seen: set[str] = set()
    normalized_metrics: list[dict[str, Any]] = []
    for index, metric in enumerate(metrics):
        if not isinstance(metric, dict):
            raise ValueError(f"metrics[{index}] must be an object")
        metric_id = str(metric.get("id", "")).strip()
        query = str(metric.get("query", "")).strip()
        if not metric_id:
            raise ValueError(f"metrics[{index}].id must not be empty")
        if metric_id in seen:
            raise ValueError(f"duplicate metric id: {metric_id}")
        if not query:
            raise ValueError(f"metrics[{index}].query must not be empty")
        seen.add(metric_id)
        normalized = dict(metric)
        normalized.setdefault("label", metric_id)
        normalized.setdefault("unit", "")
        normalized.setdefault("panel", str(normalized["label"]))
        normalized.setdefault("required", False)
        normalized_metrics.append(normalized)
    raw["metrics"] = normalized_metrics

    visualization = raw.get("visualization", {})
    if not isinstance(visualization, dict):
        raise ValueError("visualization must be an object")
    visualization.setdefault("eventOverlays", [])
    visualization.setdefault("maxOverlaySpans", 120)
    visualization.setdefault("plotlyCdn", "https://cdn.plot.ly/plotly-2.35.2.min.js")
    visualization.setdefault("heightPerPanel", 340)
    visualization.setdefault("theme", "light")
    if visualization["theme"] not in {"light", "dark"}:
        raise ValueError("visualization.theme must be either 'light' or 'dark'")
    raw["visualization"] = visualization
    return raw


def metric_specs(config: Mapping[str, Any]) -> list[MetricSpec]:
    default_step = float(config["prometheus"].get("defaultStepSeconds", DEFAULT_STEP_SECONDS))
    result: list[MetricSpec] = []
    for item in config["metrics"]:
        step_raw = item.get("stepSeconds")
        step = default_step if step_raw is None else float(step_raw)
        if step <= 0:
            raise ValueError(f"metric {item['id']}: stepSeconds must be > 0")
        result.append(
            MetricSpec(
                metric_id=str(item["id"]),
                label=str(item.get("label", item["id"])),
                query=str(item["query"]),
                unit=str(item.get("unit", "")),
                panel=str(item.get("panel", item.get("label", item["id"]))),
                step_seconds=step,
                required=bool(item.get("required", False)),
            )
        )
    return result


# ---------------------------------------------------------------------------
# Range discovery
# ---------------------------------------------------------------------------


def discover_intervals(source: Path, config: Mapping[str, Any]) -> list[Interval]:
    source_root = _find_measurement_root(source)
    ranges = config["ranges"]
    include_requests = bool(ranges.get("includeRequests", True))
    include_server = bool(ranges.get("includeServer", True))
    include_unindexed = bool(ranges.get("includeUnindexedEventFiles", False))
    include_warmups = bool(ranges.get("includeWarmups", True))

    intervals: list[Interval] = []
    covered_request_logs: set[Path] = set()

    if include_requests:
        # Accept either the whole benchmark tree (<run>/events/...) or an events
        # directory directly. The latter is important for --split-by-run-type.
        request_indexes = {p.resolve() for p in source_root.rglob("events/requests/index.json")}
        if source_root.name == "events":
            direct_index = source_root / "requests" / "index.json"
            if direct_index.is_file():
                request_indexes.add(direct_index.resolve())
        for index_path in sorted(request_indexes):
            try:
                entries = _load_json(index_path)
            except (OSError, json.JSONDecodeError):
                continue
            if not isinstance(entries, list):
                continue
            events_dir = index_path.parent.parent
            run_type = _run_type_from_events_dir(events_dir, source_root)
            for position, entry in enumerate(entries, start=1):
                if not isinstance(entry, dict):
                    continue
                request_id = str(entry.get("request_id", entry.get("id", f"request-{position:04d}")))
                measured_raw = entry.get("measured", entry.get("measure"))
                measured = None if measured_raw is None else bool(measured_raw)
                if measured is False and not include_warmups:
                    continue
                try:
                    start_ns = int(entry["started_unix_ns"])
                    end_ns = int(entry["finished_unix_ns"])
                except (KeyError, TypeError, ValueError):
                    continue
                event_file = _resolve_event_file(
                    entry.get("events_jsonl"), index_path=index_path, request_id=request_id
                )
                covered_request_logs.add(event_file.resolve())
                intervals.append(
                    Interval(
                        interval_id=f"{run_type}:request:{request_id}",
                        run_type=run_type,
                        kind="request",
                        label=request_id,
                        event_file=event_file if event_file.is_file() else None,
                        start_unix_ns=start_ns,
                        end_unix_ns=end_ns,
                        request_id=request_id,
                        measured=measured,
                    )
                )

    if include_server:
        server_files = {p.resolve() for p in source_root.rglob("events/server.events.jsonl")}
        if source_root.name == "events":
            direct_server = source_root / "server.events.jsonl"
            if direct_server.is_file():
                server_files.add(direct_server.resolve())
        for server_file in sorted(server_files):
            event_range = _event_range(server_file)
            if event_range is None:
                continue
            events_dir = server_file.parent
            run_type = _run_type_from_events_dir(events_dir, source_root)
            intervals.append(
                Interval(
                    interval_id=f"{run_type}:server",
                    run_type=run_type,
                    kind="server",
                    label="server",
                    event_file=server_file,
                    start_unix_ns=event_range[0],
                    end_unix_ns=event_range[1],
                )
            )

    if include_unindexed:
        for event_file in sorted(source_root.rglob("*.events.jsonl")):
            resolved = event_file.resolve()
            if resolved in covered_request_logs or event_file.name == "server.events.jsonl":
                continue
            event_range = _event_range(event_file)
            if event_range is None:
                continue
            # Find the nearest ancestor named events and use its parent as run type.
            events_dir = next((p for p in event_file.parents if p.name == "events"), event_file.parent)
            run_type = _run_type_from_events_dir(events_dir, source_root)
            label = event_file.name.removesuffix(".events.jsonl")
            intervals.append(
                Interval(
                    interval_id=f"{run_type}:event:{label}",
                    run_type=run_type,
                    kind="event",
                    label=label,
                    event_file=event_file,
                    start_unix_ns=event_range[0],
                    end_unix_ns=event_range[1],
                )
            )

    # Stable order: run type, request before server, then chronological.
    kind_order = {"request": 0, "event": 1, "server": 2}
    intervals.sort(
        key=lambda item: (
            item.run_type.lower(),
            kind_order.get(item.kind, 99),
            item.start_unix_ns,
            item.label,
        )
    )
    return intervals


# ---------------------------------------------------------------------------
# Prometheus query/export
# ---------------------------------------------------------------------------


def _request_headers(config: Mapping[str, Any]) -> dict[str, str]:
    headers: dict[str, str] = {"Accept": "application/json"}
    configured = config["prometheus"].get("headers", {})
    if isinstance(configured, dict):
        for key, value in configured.items():
            rendered = str(value)
            # A small convenience for local/private deployments without putting the
            # token itself in the JSON config.
            if rendered.startswith("$ENV{") and rendered.endswith("}"):
                rendered = os.environ.get(rendered[5:-1], "")
            if rendered:
                headers[str(key)] = rendered
    return headers


def query_prometheus_range(
    config: Mapping[str, Any],
    *,
    query: str,
    start_seconds: float,
    end_seconds: float,
    step_seconds: float,
) -> list[dict[str, Any]]:
    base = str(config["prometheus"]["url"]).rstrip("/")
    params = urllib.parse.urlencode(
        {
            "query": query,
            "start": f"{start_seconds:.9f}",
            "end": f"{end_seconds:.9f}",
            "step": f"{step_seconds:g}",
        }
    )
    url = f"{base}/api/v1/query_range?{params}"
    request = urllib.request.Request(url, headers=_request_headers(config))
    timeout = float(config["prometheus"].get("timeoutSeconds", DEFAULT_TIMEOUT_SECONDS))
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Prometheus HTTP {exc.code} for query {query!r}: {body}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Prometheus request failed for {query!r}: {exc}") from exc

    if payload.get("status") != "success":
        raise RuntimeError(f"Prometheus query failed for {query!r}: {payload}")
    data = payload.get("data", {})
    if data.get("resultType") not in {"matrix", "vector"}:
        raise RuntimeError(
            f"Prometheus returned unsupported resultType={data.get('resultType')!r} for {query!r}"
        )
    result = data.get("result", [])
    if not isinstance(result, list):
        raise RuntimeError(f"Prometheus returned malformed result for {query!r}")
    return result


def _series_rows(
    *,
    metric: MetricSpec,
    interval: Interval,
    result: Sequence[Mapping[str, Any]],
    query_start_seconds: float,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for series_index, series in enumerate(result):
        labels = series.get("metric", {})
        if not isinstance(labels, dict):
            labels = {}
        labels_json = json.dumps(labels, sort_keys=True, separators=(",", ":"))
        series_name = ",".join(f"{k}={v}" for k, v in sorted(labels.items())) or f"series-{series_index + 1}"
        values = series.get("values")
        if values is None and "value" in series:
            values = [series["value"]]
        if not isinstance(values, list):
            continue
        for sample in values:
            if not isinstance(sample, (list, tuple)) or len(sample) != 2:
                continue
            try:
                timestamp = float(sample[0])
                value = float(sample[1])
            except (TypeError, ValueError):
                continue
            if not math.isfinite(value):
                continue
            rows.append(
                {
                    "run_type": interval.run_type,
                    "interval_kind": interval.kind,
                    "interval_id": interval.interval_id,
                    "interval_label": interval.label,
                    "request_id": interval.request_id or "",
                    "measured": "" if interval.measured is None else str(interval.measured).lower(),
                    "metric_id": metric.metric_id,
                    "metric_label": metric.label,
                    "panel": metric.panel,
                    "unit": metric.unit,
                    "series": series_name,
                    "labels_json": labels_json,
                    "timestamp_unix_s": f"{timestamp:.9f}",
                    "timestamp_iso": _iso_from_unix_seconds(timestamp),
                    "relative_s": f"{timestamp - query_start_seconds:.9f}",
                    "value": f"{value:.17g}",
                }
            )
    return rows


def _write_csv(path: Path, rows: Sequence[Mapping[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "run_type",
        "interval_kind",
        "interval_id",
        "interval_label",
        "request_id",
        "measured",
        "metric_id",
        "metric_label",
        "panel",
        "unit",
        "series",
        "labels_json",
        "timestamp_unix_s",
        "timestamp_iso",
        "relative_s",
        "value",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def export_metrics(
    source: Path,
    config_path: Path,
    output_dir: Path,
    *,
    overwrite: bool = False,
) -> Path:
    source_root = _find_measurement_root(source)
    config_path = config_path.expanduser().resolve()
    config = load_config(config_path)
    output_dir = output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    manifest_path = output_dir / "manifest.json"
    if manifest_path.exists() and not overwrite:
        raise FileExistsError(
            f"{manifest_path} already exists; pass --overwrite to refresh the Prometheus export"
        )

    intervals = discover_intervals(source_root, config)
    if not intervals:
        raise ValueError(f"no event intervals found under {source_root}")

    metrics = metric_specs(config)
    before_ns = int(float(config["ranges"].get("beforeMs", 0)) * 1_000_000.0)
    after_ns = int(float(config["ranges"].get("afterMs", 0)) * 1_000_000.0)

    manifest_intervals: list[dict[str, Any]] = []
    total_samples = 0
    total_queries = 0
    empty_queries = 0

    for interval_index, interval in enumerate(intervals, start=1):
        query_start_ns = max(0, interval.start_unix_ns - before_ns)
        query_end_ns = max(query_start_ns, interval.end_unix_ns + after_ns)
        query_start_s = query_start_ns / 1_000_000_000.0
        query_end_s = query_end_ns / 1_000_000_000.0

        interval_dir = (
            output_dir
            / "intervals"
            / _safe_component(interval.run_type)
            / _safe_component(interval.kind)
            / _safe_component(interval.label)
        )
        interval_dir.mkdir(parents=True, exist_ok=True)

        all_rows: list[dict[str, Any]] = []
        metric_results: list[dict[str, Any]] = []
        print(
            f"[{interval_index}/{len(intervals)}] {interval.run_type} / {interval.kind} / {interval.label} "
            f"({interval.duration_seconds:.3f}s)",
            flush=True,
        )

        for metric in metrics:
            total_queries += 1
            result = query_prometheus_range(
                config,
                query=metric.query,
                start_seconds=query_start_s,
                end_seconds=query_end_s,
                step_seconds=float(metric.step_seconds or DEFAULT_STEP_SECONDS),
            )
            rows = _series_rows(
                metric=metric,
                interval=interval,
                result=result,
                query_start_seconds=query_start_s,
            )
            if not rows:
                empty_queries += 1
                if metric.required:
                    raise RuntimeError(
                        f"required metric {metric.metric_id!r} returned no samples for {interval.interval_id}"
                    )
            all_rows.extend(rows)
            total_samples += len(rows)
            metric_results.append(
                {
                    "id": metric.metric_id,
                    "label": metric.label,
                    "query": metric.query,
                    "unit": metric.unit,
                    "panel": metric.panel,
                    "stepSeconds": metric.step_seconds,
                    "seriesCount": len(result),
                    "sampleCount": len(rows),
                }
            )

        metrics_csv = interval_dir / "metrics.csv"
        _write_csv(metrics_csv, all_rows)
        interval_json = interval_dir / "interval.json"
        interval_payload = {
            "formatVersion": FORMAT_VERSION,
            "intervalId": interval.interval_id,
            "runType": interval.run_type,
            "kind": interval.kind,
            "label": interval.label,
            "requestId": interval.request_id,
            "measured": interval.measured,
            "eventFile": str(interval.event_file) if interval.event_file else None,
            "eventStartUnixNs": interval.start_unix_ns,
            "eventEndUnixNs": interval.end_unix_ns,
            "queryStartUnixNs": query_start_ns,
            "queryEndUnixNs": query_end_ns,
            "durationSeconds": interval.duration_seconds,
            "metricsCsv": str(metrics_csv),
            "metrics": metric_results,
        }
        _write_json(interval_json, interval_payload)
        manifest_intervals.append({**interval_payload, "intervalJson": str(interval_json)})

    manifest = {
        "formatVersion": FORMAT_VERSION,
        "kind": "polygraph-prometheus-event-export",
        "sourceRoot": str(source_root),
        "config": str(config_path),
        "prometheusUrl": config["prometheus"]["url"],
        "outputRoot": str(output_dir),
        "intervalCount": len(manifest_intervals),
        "queryCount": total_queries,
        "emptyQueryCount": empty_queries,
        "sampleCount": total_samples,
        "intervals": manifest_intervals,
    }
    _write_json(manifest_path, manifest)
    return manifest_path


# ---------------------------------------------------------------------------
# Event span overlays
# ---------------------------------------------------------------------------


def _base_event_name(name: str) -> tuple[str, str | None]:
    if name.endswith(".start"):
        return name[:-6], "start"
    if name.endswith(".end"):
        return name[:-4], "end"
    return name, None


def _pair_overlay_spans(path: Path, query_start_ns: int) -> list[dict[str, Any]]:
    stacks: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    spans: list[dict[str, Any]] = []
    for sequence, event in enumerate(_iter_jsonl(path), start=1):
        name = str(event.get("event", ""))
        base, edge = _base_event_name(name)
        if edge is None:
            continue
        timestamp = _event_timestamp_ns(event)
        if timestamp is None:
            continue
        thread = str(event.get("thread_id", event.get("threadId", "")))
        key = (thread, base)
        if edge == "start":
            stacks[key].append({"timestamp": timestamp, "event": event, "sequence": sequence})
            continue
        if not stacks[key]:
            continue
        start = stacks[key].pop()
        if timestamp < int(start["timestamp"]):
            continue
        spans.append(
            {
                "name": base,
                "start_s": (int(start["timestamp"]) - query_start_ns) / 1_000_000_000.0,
                "end_s": (timestamp - query_start_ns) / 1_000_000_000.0,
                "thread": thread,
            }
        )
    return spans


def _overlay_matches(name: str, configured: Sequence[str]) -> bool:
    if not configured:
        return False
    lowered = name.lower()
    for pattern in configured:
        pattern = str(pattern)
        if pattern.startswith("re:"):
            try:
                if re.search(pattern[3:], name):
                    return True
            except re.error:
                continue
        elif lowered == pattern.lower() or lowered.startswith(pattern.lower()):
            return True
    return False


# ---------------------------------------------------------------------------
# Visualization
# ---------------------------------------------------------------------------


def _read_metrics_csv(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not path.is_file():
        return rows
    with path.open("r", newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            try:
                rows.append(
                    {
                        **row,
                        "relative_s": float(row["relative_s"]),
                        "value": float(row["value"]),
                    }
                )
            except (KeyError, TypeError, ValueError):
                continue
    return rows


def _html_json(value: Any) -> str:
    return json.dumps(value, separators=(",", ":")).replace("</", "<\\/")


def _series_label_summary(labels_json: str) -> str:
    """Return only labels that help distinguish one Prometheus series."""
    try:
        labels = json.loads(labels_json)
    except (TypeError, json.JSONDecodeError):
        return ""
    if not isinstance(labels, dict):
        return ""

    noisy = {"__name__", "instance", "job", "service"}
    parts = [
        f"{key}={value}"
        for key, value in sorted(labels.items())
        if key not in noisy and str(value) != ""
    ]
    return ", ".join(parts)


def _format_metric_value(value: float, unit: str) -> str:
    """Format hover values for humans while keeping exported CSV values raw."""
    normalized = unit.strip().lower()
    if normalized in {"byte", "bytes", "b"}:
        suffixes = ("B", "KiB", "MiB", "GiB", "TiB")
        scaled = float(value)
        suffix = suffixes[0]
        for candidate in suffixes:
            suffix = candidate
            if abs(scaled) < 1024.0 or candidate == suffixes[-1]:
                break
            scaled /= 1024.0
        precision = 0 if abs(scaled) >= 100 else 1 if abs(scaled) >= 10 else 2
        return f"{scaled:.{precision}f} {suffix}"

    if normalized in {"second", "seconds", "s"}:
        absolute = abs(value)
        if absolute < 0.001:
            return f"{value * 1_000_000.0:.3g} µs"
        if absolute < 1.0:
            return f"{value * 1_000.0:.3g} ms"
        return f"{value:.4g} s"

    if normalized in {"percent", "%"}:
        return f"{value:.3g} %"

    suffix = f" {unit}" if unit else ""
    return f"{value:.4g}{suffix}"


def _metric_default_visible(metric: Mapping[str, Any]) -> bool:
    explicit = metric.get("defaultVisible")
    if explicit is not None:
        return bool(explicit)
    searchable = " ".join(
        str(metric.get(key, ""))
        for key in ("id", "label", "panel")
    ).lower()
    return "http" not in searchable


def _write_interval_page(
    *,
    output_path: Path,
    interval: Mapping[str, Any],
    rows: Sequence[Mapping[str, Any]],
    config: Mapping[str, Any],
    index_href: str,
) -> None:
    visualization = config["visualization"]
    overlays: list[dict[str, Any]] = []
    event_file_raw = interval.get("eventFile")
    if event_file_raw:
        event_file = Path(str(event_file_raw))
        if event_file.is_file():
            overlays = _pair_overlay_spans(event_file, int(interval["queryStartUnixNs"]))
            configured = visualization.get("eventOverlays", [])
            overlays = [span for span in overlays if _overlay_matches(span["name"], configured)]
            overlays = overlays[: int(visualization.get("maxOverlaySpans", 120))]

    metric_config = {
        str(metric["id"]): metric
        for metric in config.get("metrics", [])
        if isinstance(metric, Mapping) and metric.get("id")
    }

    # Split data into panels and traces so the browser receives already compact data.
    panels: dict[str, dict[str, Any]] = {}
    grouped: dict[tuple[str, str, str], list[Mapping[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[(str(row["panel"]), str(row["metric_id"]), str(row["series"]))].append(row)

    metric_series_counts: dict[tuple[str, str], int] = defaultdict(int)
    for panel, metric_id, _series in grouped:
        metric_series_counts[(panel, metric_id)] += 1

    present_metric_ids: set[str] = set()
    for (panel, metric_id, series), samples in grouped.items():
        samples = sorted(samples, key=lambda row: float(row["relative_s"]))
        entry = panels.setdefault(panel, {"traces": [], "units": set()})
        metric_label = str(samples[0].get("metric_label", metric_id))
        unit = str(samples[0].get("unit", ""))
        labels_json = str(samples[0].get("labels_json", "{}"))
        series_summary = _series_label_summary(labels_json)
        name = metric_label
        if metric_series_counts[(panel, metric_id)] > 1:
            name += f" — {series_summary or series}"

        metric_definition = metric_config.get(metric_id, {})
        default_visible = _metric_default_visible(metric_definition or {
            "id": metric_id,
            "label": metric_label,
            "panel": panel,
        })
        present_metric_ids.add(metric_id)
        if unit:
            entry["units"].add(unit)
        values = [float(sample["value"]) for sample in samples]
        entry["traces"].append(
            {
                "name": name,
                "metricId": metric_id,
                "unit": unit,
                "seriesSummary": series_summary,
                "defaultVisible": default_visible,
                "x": [float(sample["relative_s"]) for sample in samples],
                "y": values,
                "hover": [_format_metric_value(value, unit) for value in values],
            }
        )

    panel_payload = []
    for name, value in panels.items():
        units = sorted(value.pop("units"))
        panel_payload.append({
            "name": name,
            "unit": units[0] if len(units) == 1 else "",
            **value,
        })

    metric_controls = []
    for metric in config.get("metrics", []):
        if not isinstance(metric, Mapping):
            continue
        metric_id = str(metric.get("id", ""))
        if metric_id not in present_metric_ids:
            continue
        metric_controls.append({
            "id": metric_id,
            "label": str(metric.get("label", metric_id)),
            "panel": str(metric.get("panel", "")),
            "defaultVisible": _metric_default_visible(metric),
        })

    plotly_src = html.escape(str(visualization.get("plotlyCdn")))
    height = int(visualization.get("heightPerPanel", 340))
    theme = str(visualization.get("theme", "light"))
    if theme == "dark":
        page_colors = {
            "color_scheme": "dark",
            "bg": "#0b0f14",
            "card": "#111821",
            "border": "#263140",
            "text": "#e5edf7",
            "muted": "#93a4b8",
            "link": "#9bc5ff",
            "header": "rgba(11,15,20,0.93)",
            "grid": "#243041",
            "zeroline": "#334155",
            "overlay": "rgba(130,160,210,0.08)",
            "chip": "#18212c",
            "chip_checked": "#18385f",
            "button": "#18212c",
        }
    else:
        page_colors = {
            "color_scheme": "light",
            "bg": "#f4f7fb",
            "card": "#ffffff",
            "border": "#d8e0ea",
            "text": "#172033",
            "muted": "#64748b",
            "link": "#2563eb",
            "header": "rgba(255,255,255,0.94)",
            "grid": "#e2e8f0",
            "zeroline": "#cbd5e1",
            "overlay": "rgba(37,99,235,0.055)",
            "chip": "#f8fafc",
            "chip_checked": "#dbeafe",
            "button": "#f8fafc",
        }
    title = f"{interval['runType']} / {interval['kind']} / {interval['label']}"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    document = f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>{html.escape(title)} — Prometheus metrics</title>
  <script src="{plotly_src}"></script>
  <style>
    :root {{ color-scheme:{page_colors["color_scheme"]}; --bg:{page_colors["bg"]}; --card:{page_colors["card"]}; --border:{page_colors["border"]}; --text:{page_colors["text"]}; --muted:{page_colors["muted"]}; }}
    * {{ box-sizing:border-box; }}
    body {{ margin:0; background:var(--bg); color:var(--text); font:14px/1.45 system-ui,sans-serif; }}
    header {{ position:sticky; top:0; z-index:5; background:{page_colors["header"]}; backdrop-filter:blur(10px); border-bottom:1px solid var(--border); padding:14px 20px; display:flex; gap:16px; align-items:center; }}
    header a {{ color:{page_colors["link"]}; text-decoration:none; }}
    header h1 {{ margin:0; font-size:18px; }}
    header .meta {{ margin-left:auto; color:var(--muted); font-size:12px; }}
    .metric-toolbar {{ position:sticky; top:49px; z-index:4; border-bottom:1px solid var(--border); background:{page_colors["header"]}; backdrop-filter:blur(10px); padding:10px 18px 12px; }}
    .metric-toolbar-heading {{ display:flex; align-items:center; justify-content:space-between; gap:12px; margin-bottom:8px; }}
    .metric-toolbar-heading strong {{ font-size:13px; }}
    .metric-toolbar-heading span {{ color:var(--muted); font-size:12px; margin-left:8px; }}
    .metric-actions {{ display:flex; gap:6px; }}
    .metric-actions button {{ border:1px solid var(--border); border-radius:7px; padding:5px 9px; background:{page_colors["button"]}; color:var(--text); cursor:pointer; font-weight:650; }}
    .metric-actions button:hover {{ border-color:{page_colors["link"]}; }}
    .metric-chips {{ display:flex; flex-wrap:wrap; gap:7px; }}
    .metric-chip {{ display:inline-flex; align-items:center; gap:7px; border:1px solid var(--border); border-radius:999px; padding:6px 9px; background:{page_colors["chip"]}; color:var(--text); cursor:pointer; user-select:none; }}
    .metric-chip:has(input:checked) {{ border-color:{page_colors["link"]}; background:{page_colors["chip_checked"]}; }}
    .metric-chip input {{ margin:0; accent-color:{page_colors["link"]}; }}
    .metric-chip small {{ color:var(--muted); font-size:10px; }}
    main {{ padding:18px; display:grid; gap:14px; }}
    .panel {{ background:var(--card); border:1px solid var(--border); border-radius:12px; padding:10px; }}
    .panel[hidden] {{ display:none; }}
    .panel h2 {{ margin:4px 8px 0; font-size:15px; }}
    .plot {{ height:{height}px; }}
    .empty {{ color:var(--muted); padding:40px; text-align:center; }}
  </style>
</head>
<body>
<header>
  <a href="{html.escape(index_href)}">← Metrics index</a>
  <h1>{html.escape(title)}</h1>
  <div class="meta">event window {float(interval['durationSeconds']):.3f}s</div>
</header>
<div class="metric-toolbar" id="metricToolbar" hidden>
  <div class="metric-toolbar-heading">
    <div><strong>Visible metrics</strong><span>Toggle whole metrics; individual Prometheus series can still be toggled from each chart legend.</span></div>
    <div class="metric-actions">
      <button type="button" data-metric-action="defaults">Defaults</button>
      <button type="button" data-metric-action="all">All</button>
      <button type="button" data-metric-action="none">None</button>
    </div>
  </div>
  <div class="metric-chips" id="metricChips"></div>
</div>
<main id="panels"></main>
<script>
const panels = {_html_json(panel_payload)};
const metricDefinitions = {_html_json(metric_controls)};
const overlays = {_html_json(overlays)};
const host = document.getElementById('panels');
const metricToolbar = document.getElementById('metricToolbar');
const metricChips = document.getElementById('metricChips');
const metricSections = [];

function selectedMetricIds() {{
  return new Set(
    [...document.querySelectorAll('[data-metric-toggle]:checked')].map(input => input.value)
  );
}}

function metricTrace(trace, enabled) {{
  return {{
    type:'scatter', mode:'lines', name:trace.name,
    x:trace.x, y:trace.y, text:trace.hover,
    visible:enabled,
    meta:{{metricId:trace.metricId}},
    hovertemplate:'<b>%{{fullData.name}}</b>: %{{text}}<extra></extra>'
  }};
}}

function metricLayout(panel) {{
  const shapes = overlays.map(span => ({{
    type:'rect', xref:'x', yref:'paper', x0:span.start_s, x1:span.end_s, y0:0, y1:1,
    fillcolor:'{page_colors["overlay"]}', line:{{width:0}}, layer:'below'
  }}));
  return {{
    margin:{{l:72,r:24,t:18,b:58}}, paper_bgcolor:'transparent', plot_bgcolor:'transparent',
    font:{{color:'{page_colors["text"]}'}}, hovermode:'x unified',
    hoverlabel:{{bgcolor:'{page_colors["card"]}', bordercolor:'{page_colors["border"]}', font:{{color:'{page_colors["text"]}'}}}},
    legend:{{orientation:'h', y:-0.23}},
    xaxis:{{title:'Time relative to query window start [s]', hoverformat:'.2f', gridcolor:'{page_colors["grid"]}', zerolinecolor:'{page_colors["zeroline"]}'}},
    yaxis:{{title:panel.unit || undefined, gridcolor:'{page_colors["grid"]}', zerolinecolor:'{page_colors["zeroline"]}', tickformat:'~s'}},
    shapes
  }};
}}

function renderMetricSections() {{
  const enabled = selectedMetricIds();
  for (const item of metricSections) {{
    const visible = item.panel.traces.map(trace => enabled.has(trace.metricId));
    const anyVisible = visible.some(Boolean);
    item.section.hidden = !anyVisible;
    if (!anyVisible) continue;
    const traces = item.panel.traces.map((trace, index) => metricTrace(trace, visible[index]));
    Plotly.react(item.plot, traces, metricLayout(item.panel), {{responsive:true, displaylogo:false}});
  }}
}}

function setMetricSelection(mode) {{
  document.querySelectorAll('[data-metric-toggle]').forEach(input => {{
    if (mode === 'all') input.checked = true;
    else if (mode === 'none') input.checked = false;
    else input.checked = input.dataset.defaultVisible === 'true';
  }});
  renderMetricSections();
}}

if (metricDefinitions.length) {{
  metricToolbar.hidden = false;
  for (const metric of metricDefinitions) {{
    const label = document.createElement('label');
    label.className = 'metric-chip';
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.value = metric.id;
    input.dataset.metricToggle = '';
    input.dataset.defaultVisible = metric.defaultVisible ? 'true' : 'false';
    input.checked = Boolean(metric.defaultVisible);
    input.addEventListener('change', renderMetricSections);
    const text = document.createElement('span');
    text.textContent = metric.label;
    const panelName = document.createElement('small');
    panelName.textContent = metric.panel;
    label.append(input, text, panelName);
    metricChips.appendChild(label);
  }}
  document.querySelectorAll('[data-metric-action]').forEach(button => {{
    button.addEventListener('click', () => setMetricSelection(button.dataset.metricAction));
  }});
}}

if (overlays.length) {{
  const section = document.createElement('section');
  section.className = 'panel';
  const heading = document.createElement('h2');
  heading.textContent = 'Event spans';
  const plot = document.createElement('div');
  plot.className = 'plot';
  section.append(heading, plot); host.appendChild(section);
  const names = [...new Set(overlays.map(span => span.name))];
  const traces = names.map(name => {{
    const x = [], y = [], customdata = [];
    for (const span of overlays.filter(item => item.name === name)) {{
      x.push(span.start_s, span.end_s, null);
      y.push(name, name, name);
      customdata.push([span.start_s, span.end_s], [span.start_s, span.end_s], [null, null]);
    }}
    return {{
      type:'scatter', mode:'lines', name, x, y, customdata,
      line:{{width:10}},
      hovertemplate:'<b>%{{fullData.name}}</b><br>%{{customdata[0]:.3f}}–%{{customdata[1]:.3f}} s<extra></extra>'
    }};
  }});
  Plotly.newPlot(plot, traces, {{
    margin:{{l:160,r:24,t:18,b:55}}, paper_bgcolor:'transparent', plot_bgcolor:'transparent',
    font:{{color:'{page_colors["text"]}'}}, showlegend:false,
    hoverlabel:{{bgcolor:'{page_colors["card"]}', bordercolor:'{page_colors["border"]}', font:{{color:'{page_colors["text"]}'}}}},
    xaxis:{{title:'Time relative to query window start [s]', hoverformat:'.2f', gridcolor:'{page_colors["grid"]}', zerolinecolor:'{page_colors["zeroline"]}'}},
    yaxis:{{automargin:true, gridcolor:'{page_colors["grid"]}', zerolinecolor:'{page_colors["zeroline"]}'}}
  }}, {{responsive:true, displaylogo:false}});
}}

for (const panel of panels) {{
  const section = document.createElement('section');
  section.className = 'panel';
  section.dataset.metricPanel = panel.name;
  const heading = document.createElement('h2');
  heading.textContent = panel.name;
  const plot = document.createElement('div');
  plot.className = 'plot';
  section.append(heading, plot); host.appendChild(section);
  metricSections.push({{section, plot, panel}});
}}
renderMetricSections();

if (!panels.length && !overlays.length) {{
  host.innerHTML = '<div class="empty">No Prometheus samples or configured event overlays in this interval.</div>';
}}
</script>
</body>
</html>
"""
    output_path.write_text(document, encoding="utf-8")

def visualize_metrics(
    manifest_path: Path,
    *,
    output_dir: Path | None = None,
    config_path: Path | None = None,
) -> Path:
    manifest_path = manifest_path.expanduser().resolve()
    manifest = _load_json(manifest_path)
    if manifest.get("kind") != "polygraph-prometheus-event-export":
        raise ValueError(f"not an event metrics export manifest: {manifest_path}")
    if config_path is None:
        config_path = Path(str(manifest["config"]))
    config = load_config(config_path)
    if output_dir is None:
        output_dir = manifest_path.parent / "visualization"
    output_dir = output_dir.expanduser().resolve()
    pages_root = output_dir / "intervals"
    output_dir.mkdir(parents=True, exist_ok=True)

    page_entries: list[dict[str, Any]] = []
    for interval in manifest.get("intervals", []):
        metrics_csv = Path(str(interval["metricsCsv"]))
        rows = _read_metrics_csv(metrics_csv)
        page = (
            pages_root
            / _safe_component(str(interval["runType"]))
            / _safe_component(str(interval["kind"]))
            / f"{_safe_component(str(interval['label']))}.html"
        )
        index_href = os.path.relpath(output_dir / "index.html", page.parent).replace(os.sep, "/")
        _write_interval_page(
            output_path=page,
            interval=interval,
            rows=rows,
            config=config,
            index_href=index_href,
        )
        page_entries.append(
            {
                "runType": interval["runType"],
                "kind": interval["kind"],
                "label": interval["label"],
                "measured": interval.get("measured"),
                "durationSeconds": interval.get("durationSeconds", 0),
                "href": page.relative_to(output_dir).as_posix(),
                "sampleCount": sum(int(metric.get("sampleCount", 0)) for metric in interval.get("metrics", [])),
            }
        )

    theme = str(config["visualization"].get("theme", "light"))
    if theme == "dark":
        index_colors = {
            "scheme": "dark", "bg": "#0b0f14", "card": "#111821",
            "border": "#263140", "text": "#e5edf7", "muted": "#93a4b8",
            "hover": "#5476a0", "tag_border": "#6d5a2a", "tag": "#e7c76f",
        }
    else:
        index_colors = {
            "scheme": "light", "bg": "#f4f7fb", "card": "#ffffff",
            "border": "#d8e0ea", "text": "#172033", "muted": "#64748b",
            "hover": "#93b4df", "tag_border": "#d6b75a", "tag": "#8a6500",
        }

    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for entry in page_entries:
        grouped[str(entry["runType"])].append(entry)

    sections: list[str] = []
    for run_type, entries in sorted(grouped.items(), key=lambda item: item[0].lower()):
        cards = []
        for entry in entries:
            warmup = " <span class=\"tag\">warmup</span>" if entry.get("measured") is False else ""
            cards.append(
                f'<a class="card" href="{html.escape(entry["href"])}">'
                f'<strong>{html.escape(str(entry["label"]))}</strong>{warmup}'
                f'<span>{html.escape(str(entry["kind"]))} · {float(entry["durationSeconds"]):.3f}s · '
                f'{int(entry["sampleCount"]):,} samples</span></a>'
            )
        sections.append(f"<section><h2>{html.escape(run_type)}</h2><div class=\"grid\">{''.join(cards)}</div></section>")

    index = output_dir / "index.html"
    index.write_text(
        f"""<!doctype html><html><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>PolyGraph Prometheus event metrics</title><style>
:root{{color-scheme:{index_colors["scheme"]};--bg:{index_colors["bg"]};--card:{index_colors["card"]};--border:{index_colors["border"]};--text:{index_colors["text"]};--muted:{index_colors["muted"]}}}*{{box-sizing:border-box}}body{{margin:0;padding:24px;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,sans-serif}}h1{{margin-top:0}}h2{{margin-top:28px}}.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:10px}}.card{{display:flex;flex-direction:column;gap:6px;padding:14px;background:var(--card);border:1px solid var(--border);border-radius:10px;color:var(--text);text-decoration:none;box-shadow:0 1px 2px rgba(15,23,42,.04)}}.card:hover{{border-color:{index_colors["hover"]};box-shadow:0 4px 14px rgba(15,23,42,.08)}}.card span{{color:var(--muted);font-size:12px}}.tag{{display:inline-block;margin-left:7px;padding:1px 5px;border:1px solid {index_colors["tag_border"]};border-radius:5px;color:{index_colors["tag"]}!important}}
</style></head><body><h1>PolyGraph Prometheus event metrics</h1><p>One page per event interval. Request ranges use the exact HTTP start/end timestamps from <code>events/requests/index.json</code> when available.</p>{''.join(sections)}</body></html>""",
        encoding="utf-8",
    )
    _write_json(
        output_dir / "visualization.json",
        {
            "formatVersion": FORMAT_VERSION,
            "manifest": str(manifest_path),
            "index": str(index),
            "pageCount": len(page_entries),
            "pages": page_entries,
        },
    )
    return index


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    export = commands.add_parser("export", help="Pull configured Prometheus metrics for event-log ranges.")
    export.add_argument("source", type=Path, help="Benchmark/run tree containing events directories.")
    export.add_argument("--config", type=Path, required=True)
    export.add_argument("--out-dir", type=Path, required=True)
    export.add_argument("--overwrite", action="store_true")

    visualize = commands.add_parser("visualize", help="Generate lightweight HTML pages from an existing export.")
    visualize.add_argument("manifest", type=Path)
    visualize.add_argument("--config", type=Path)
    visualize.add_argument("--out-dir", type=Path)

    all_cmd = commands.add_parser("all", help="Export from Prometheus and immediately generate HTML visualization.")
    all_cmd.add_argument("source", type=Path)
    all_cmd.add_argument("--config", type=Path, required=True)
    all_cmd.add_argument("--out-dir", type=Path, required=True, help="Export directory; visualization is written below it.")
    all_cmd.add_argument("--overwrite", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "export":
            manifest = export_metrics(args.source, args.config, args.out_dir, overwrite=args.overwrite)
            print(f"Metrics manifest: {manifest}")
            return 0
        if args.command == "visualize":
            index = visualize_metrics(args.manifest, output_dir=args.out_dir, config_path=args.config)
            print(f"Metrics dashboard: {index}")
            return 0
        if args.command == "all":
            manifest = export_metrics(args.source, args.config, args.out_dir, overwrite=args.overwrite)
            index = visualize_metrics(manifest, output_dir=args.out_dir / "visualization", config_path=args.config)
            print(f"Metrics manifest:  {manifest}")
            print(f"Metrics dashboard: {index}")
            return 0
        raise AssertionError(args.command)
    except Exception as exc:  # noqa: BLE001
        print(f"event_metrics: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
