#!/usr/bin/env python3
"""Summarize ArangoDB BFS request and scheduler events from PolyGraph JSONL."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean
from typing import Any, Iterable


REQUEST_EVENT_BASES = {
    "arango.stream.edge_index_query",
    "arango.minimal_neighbors.edge_index_query",
}


def _number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def _percentile(values: Iterable[float], percentile: float) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * percentile
    lower = int(math.floor(rank))
    upper = int(math.ceil(rank))
    if lower == upper:
        return ordered[lower]
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _stats(values: list[float]) -> dict[str, float | int | None]:
    return {
        "count": len(values),
        "mean": mean(values) if values else None,
        "p50": _percentile(values, 0.50),
        "p95": _percentile(values, 0.95),
        "max": max(values) if values else None,
    }


def _resolve_events_path(value: str | Path) -> Path:
    path = Path(value).expanduser().resolve()
    if path.is_file():
        return path
    candidates = [
        path / "events" / "server.events.jsonl",
        path / "server.events.jsonl",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        f"Could not find server.events.jsonl under {path}; pass the JSONL file directly."
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Summarize individual Arango edge-index request latency/rows and "
            "BackendRequestScheduler queue delay from PolyGraph event JSONL."
        )
    )
    parser.add_argument("source", help="Run directory or server.events.jsonl")
    parser.add_argument("--out-prefix", help="Optional output path prefix for .json and .csv")
    args = parser.parse_args()

    source = _resolve_events_path(args.source)
    request_records: list[dict[str, Any]] = []
    queue_waits: list[float] = []

    with source.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(event, dict):
                continue
            name = str(event.get("event", ""))

            if name == "backend.scheduler.dequeue":
                backend = str(event.get("backend", "")).lower()
                if "arango" in backend:
                    wait = _number(event.get("queue_wait_ms"))
                    if wait is not None:
                        queue_waits.append(wait)
                continue

            if not name.endswith(".end"):
                continue
            base = name[:-4]
            if base not in REQUEST_EVENT_BASES:
                continue

            duration = _number(event.get("duration_ms"))
            rows = _number(event.get("response_row_count"))
            group_size = _number(event.get("group_size"))
            request_bytes = _number(event.get("request_bytes"))
            attempts = _number(event.get("attempts"))
            if duration is None:
                continue

            request_records.append(
                {
                    "event": base,
                    "duration_ms": duration,
                    "response_rows": rows,
                    "group_size": group_size,
                    "request_bytes": request_bytes,
                    "attempts": attempts,
                    "edge_collection": event.get("edge_collection"),
                    "vertex_collection": event.get("vertex_collection"),
                    "direction": event.get("edge_direction"),
                    "worker_index": event.get("worker_index"),
                    "batch_index": event.get("batch_index"),
                    "request_strategy": event.get("request_strategy"),
                    "line_number": line_number,
                }
            )

    durations = [float(record["duration_ms"]) for record in request_records]
    rows = [
        float(record["response_rows"])
        for record in request_records
        if record["response_rows"] is not None
    ]
    request_bytes = [
        float(record["request_bytes"])
        for record in request_records
        if record["request_bytes"] is not None
    ]
    attempts = [
        float(record["attempts"])
        for record in request_records
        if record["attempts"] is not None
    ]

    total_rows = sum(rows)
    total_request_ms = sum(durations)
    summary = {
        "source": str(source),
        "request_count": len(request_records),
        "request_duration_ms": _stats(durations),
        "response_rows": _stats(rows),
        "request_bytes": _stats(request_bytes),
        "attempts": _stats(attempts),
        "scheduler_queue_wait_ms": _stats(queue_waits),
        "total_response_rows": total_rows,
        "sum_request_duration_ms": total_request_ms,
        # This is service work efficiency, not end-to-end throughput; concurrent
        # request durations overlap.
        "rows_per_request_second": (
            total_rows / (total_request_ms / 1000.0)
            if total_request_ms > 0.0
            else None
        ),
    }

    print(json.dumps(summary, indent=2))

    if args.out_prefix:
        prefix = Path(args.out_prefix).expanduser().resolve()
        prefix.parent.mkdir(parents=True, exist_ok=True)
        prefix.with_suffix(".json").write_text(
            json.dumps(summary, indent=2) + "\n",
            encoding="utf-8",
        )
        fields = [
            "event",
            "duration_ms",
            "response_rows",
            "group_size",
            "request_bytes",
            "attempts",
            "edge_collection",
            "vertex_collection",
            "direction",
            "worker_index",
            "batch_index",
            "request_strategy",
            "line_number",
        ]
        with prefix.with_suffix(".csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerows(request_records)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
