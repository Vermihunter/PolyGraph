#!/usr/bin/env python3
"""Serve the existing thread timeline while a JSONL event log is being written.

The server intentionally uses only the Python standard library for HTTP/SSE. It
reuses ``log_reconstruction.py`` for event normalization, span attributes, and
final static HTML generation.
"""

from __future__ import annotations

import argparse
import copy
import html
import json
import queue
import re
import signal
import sys
import threading
import time
import webbrowser
from collections import defaultdict
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

import pandas as pd

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.event_log_average import normalize_event
from tools.measurement.log_reconstruction import (
    build_dashboard,
    base_event_name,
    extract_event_attributes,
    make_safe_html_id,
    merge_span_attributes,
)


@dataclass
class LiveSpan:
    span_id: str
    name: str
    thread_id: Any
    thread_index: int
    lane: int
    seq_start: int
    start_ns: int
    start_event: dict[str, Any]
    attributes: dict[str, Any]
    seq_end: int | None = None
    end_ns: int | None = None
    end_event: dict[str, Any] | None = None

    @property
    def running(self) -> bool:
        return self.end_ns is None

    def as_message(self) -> dict[str, Any]:
        duration_ns = None if self.end_ns is None else self.end_ns - self.start_ns
        return {
            "id": self.span_id,
            "name": self.name,
            "threadId": self.thread_id,
            "threadIndex": self.thread_index,
            "lane": self.lane,
            "seqStart": self.seq_start,
            "seqEnd": self.seq_end,
            "startNs": self.start_ns,
            "endNs": self.end_ns,
            "durationNs": duration_ns,
            "running": self.running,
            "attributes": copy.deepcopy(self.attributes),
        }


@dataclass
class LiveStateSnapshot:
    revision: int
    events: list[dict[str, Any]]
    completed_spans: list[dict[str, Any]]
    open_spans: list[dict[str, Any]]
    first_start_ns: int | None
    estimated_elapsed_ns: int
    unmatched_end_count: int
    parse_error_count: int
    source_path: str

    def as_message(self) -> dict[str, Any]:
        return {
            "type": "snapshot",
            "revision": self.revision,
            "sourcePath": self.source_path,
            "events": self.events,
            "completedSpans": self.completed_spans,
            "openSpans": self.open_spans,
            "firstStartNs": self.first_start_ns,
            "estimatedElapsedNs": self.estimated_elapsed_ns,
            "unmatchedEndCount": self.unmatched_end_count,
            "parseErrorCount": self.parse_error_count,
        }


class LiveReconstructor:
    """Incrementally pair start/end events and allocate overlap lanes."""

    def __init__(self, source_path: Path):
        self.source_path = source_path
        self.lock = threading.RLock()
        self.revision = 0
        self.events: list[dict[str, Any]] = []
        self.completed_spans: list[LiveSpan] = []
        self.open_by_id: dict[str, LiveSpan] = {}
        self.stacks: dict[tuple[Any, str], list[str]] = defaultdict(list)
        self.thread_indices: dict[Any, int] = {}
        self.active_lanes: dict[Any, set[int]] = defaultdict(set)
        self.first_start_ns: int | None = None
        self.last_event_elapsed_ns = 0
        self.last_event_received_monotonic_ns = time.monotonic_ns()
        self.last_source_activity_monotonic_ns = self.last_event_received_monotonic_ns
        self.unmatched_end_count = 0
        self.parse_error_count = 0
        self.dirty_since_export = False

    def _next_revision(self) -> int:
        self.revision += 1
        return self.revision

    def _thread_index(self, thread_id: Any) -> int:
        if thread_id not in self.thread_indices:
            self.thread_indices[thread_id] = len(self.thread_indices)
        return self.thread_indices[thread_id]

    def _allocate_lane(self, thread_id: Any) -> int:
        used = self.active_lanes[thread_id]
        lane = 0
        while lane in used:
            lane += 1
        used.add(lane)
        return lane

    def _release_lane(self, thread_id: Any, lane: int) -> None:
        self.active_lanes[thread_id].discard(lane)

    def process_event(self, raw_event: dict[str, Any], *, line_number: int) -> list[dict[str, Any]]:
        normalized = normalize_event(
            raw_event,
            path=self.source_path,
            line_number=line_number,
        )
        event_name = str(normalized.get("event", ""))
        if not event_name:
            raise ValueError(f"{self.source_path}:{line_number}: event name is missing")
        if "thread_id" not in normalized:
            raise ValueError(f"{self.source_path}:{line_number}: thread_id is missing")

        messages: list[dict[str, Any]] = []
        with self.lock:
            event = copy.deepcopy(normalized)
            self.events.append(event)
            elapsed_ns = int(event["steady_ns"])
            self.last_event_elapsed_ns = max(self.last_event_elapsed_ns, elapsed_ns)
            now = time.monotonic_ns()
            self.last_event_received_monotonic_ns = now
            self.last_source_activity_monotonic_ns = now
            self.dirty_since_export = True

            thread_id = event["thread_id"]
            name = base_event_name(event_name)
            key = (thread_id, name)

            if event_name.endswith(".start"):
                seq = int(event["seq"])
                span_id = f"{thread_id}:{name}:{seq}"
                lane = self._allocate_lane(thread_id)
                span = LiveSpan(
                    span_id=span_id,
                    name=name,
                    thread_id=thread_id,
                    thread_index=self._thread_index(thread_id),
                    lane=lane,
                    seq_start=seq,
                    start_ns=elapsed_ns,
                    start_event=event,
                    attributes=extract_event_attributes(event),
                )
                self.first_start_ns = (
                    span.start_ns
                    if self.first_start_ns is None
                    else min(self.first_start_ns, span.start_ns)
                )
                self.open_by_id[span_id] = span
                self.stacks[key].append(span_id)
                revision = self._next_revision()
                messages.append({
                    "type": "span_opened",
                    "revision": revision,
                    "span": span.as_message(),
                    "estimatedElapsedNs": self._estimated_elapsed_ns_locked(),
                })

            elif event_name.endswith(".end"):
                if not self.stacks[key]:
                    self.unmatched_end_count += 1
                    revision = self._next_revision()
                    messages.append({
                        "type": "unmatched_end",
                        "revision": revision,
                        "event": self._public_event(event),
                        "unmatchedEndCount": self.unmatched_end_count,
                    })
                else:
                    span_id = self.stacks[key].pop()
                    span = self.open_by_id.pop(span_id)
                    span.seq_end = int(event["seq"])
                    span.end_ns = elapsed_ns
                    span.end_event = event
                    span.attributes = merge_span_attributes(span.start_event, event)
                    self._release_lane(thread_id, span.lane)
                    if span.end_ns >= span.start_ns:
                        self.completed_spans.append(span)
                        revision = self._next_revision()
                        messages.append({
                            "type": "span_completed",
                            "revision": revision,
                            "span": span.as_message(),
                            "estimatedElapsedNs": self._estimated_elapsed_ns_locked(),
                        })
                    else:
                        revision = self._next_revision()
                        messages.append({
                            "type": "invalid_span",
                            "revision": revision,
                            "spanId": span.span_id,
                            "reason": "end timestamp precedes start timestamp",
                        })
            else:
                revision = self._next_revision()
                messages.append({
                    "type": "event",
                    "revision": revision,
                    "event": self._public_event(event),
                    "estimatedElapsedNs": self._estimated_elapsed_ns_locked(),
                })

        return messages

    @staticmethod
    def _public_event(event: dict[str, Any]) -> dict[str, Any]:
        return {
            "event": event.get("event"),
            "seq": event.get("seq"),
            "steadyNs": event.get("steady_ns"),
            "threadId": event.get("thread_id"),
        }

    def reset(self) -> LiveStateSnapshot:
        """Reset reconstruction state when the watched file is replaced/truncated."""
        with self.lock:
            self.events.clear()
            self.completed_spans.clear()
            self.open_by_id.clear()
            self.stacks.clear()
            self.thread_indices.clear()
            self.active_lanes.clear()
            self.first_start_ns = None
            self.last_event_elapsed_ns = 0
            now = time.monotonic_ns()
            self.last_event_received_monotonic_ns = now
            self.last_source_activity_monotonic_ns = now
            self.unmatched_end_count = 0
            self.parse_error_count = 0
            self.dirty_since_export = False
            self._next_revision()
            return LiveStateSnapshot(
                revision=self.revision,
                events=[],
                completed_spans=[],
                open_spans=[],
                first_start_ns=None,
                estimated_elapsed_ns=0,
                unmatched_end_count=0,
                parse_error_count=0,
                source_path=str(self.source_path),
            )

    def record_parse_error(self) -> dict[str, Any]:
        with self.lock:
            self.parse_error_count += 1
            revision = self._next_revision()
            return {
                "type": "parse_error",
                "revision": revision,
                "parseErrorCount": self.parse_error_count,
            }

    def _estimated_elapsed_ns_locked(self) -> int:
        delta = max(0, time.monotonic_ns() - self.last_event_received_monotonic_ns)
        return self.last_event_elapsed_ns + delta

    def estimated_elapsed_ns(self) -> int:
        with self.lock:
            return self._estimated_elapsed_ns_locked()

    def open_span_count(self) -> int:
        with self.lock:
            return len(self.open_by_id)

    def idle_seconds(self) -> float:
        with self.lock:
            return max(
                0.0,
                (time.monotonic_ns() - self.last_source_activity_monotonic_ns) / 1e9,
            )

    def snapshot(self) -> LiveStateSnapshot:
        with self.lock:
            return LiveStateSnapshot(
                revision=self.revision,
                events=copy.deepcopy(self.events),
                completed_spans=[span.as_message() for span in self.completed_spans],
                open_spans=[span.as_message() for span in self.open_by_id.values()],
                first_start_ns=self.first_start_ns,
                estimated_elapsed_ns=self._estimated_elapsed_ns_locked(),
                unmatched_end_count=self.unmatched_end_count,
                parse_error_count=self.parse_error_count,
                source_path=str(self.source_path),
            )

    def mark_exported(self) -> None:
        with self.lock:
            self.dirty_since_export = False

    def should_idle_export(self, idle_seconds: float) -> bool:
        with self.lock:
            if idle_seconds <= 0 or not self.dirty_since_export:
                return False
            if self.open_by_id:
                return False
            return (
                time.monotonic_ns() - self.last_source_activity_monotonic_ns
            ) / 1e9 >= idle_seconds


class SSEBroadcaster:
    def __init__(self):
        self.lock = threading.Lock()
        self.clients: set[queue.Queue[dict[str, Any]]] = set()

    def register(self) -> queue.Queue[dict[str, Any]]:
        client: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=4096)
        with self.lock:
            self.clients.add(client)
        return client

    def unregister(self, client: queue.Queue[dict[str, Any]]) -> None:
        with self.lock:
            self.clients.discard(client)

    def publish(self, message: dict[str, Any]) -> None:
        stale: list[queue.Queue[dict[str, Any]]] = []
        with self.lock:
            for client in self.clients:
                try:
                    client.put_nowait(message)
                except queue.Full:
                    stale.append(client)
            for client in stale:
                self.clients.discard(client)


class JsonlTailer(threading.Thread):
    def __init__(
        self,
        path: Path,
        state: LiveReconstructor,
        broadcaster: SSEBroadcaster,
        stop_event: threading.Event,
        poll_interval: float,
    ):
        super().__init__(name="jsonl-tailer", daemon=True)
        self.path = path
        self.state = state
        self.broadcaster = broadcaster
        self.stop_event = stop_event
        self.poll_interval = poll_interval
        self.offset = 0
        self.partial = ""
        self.line_number = 0
        self.inode: int | None = None

    def _reset(self) -> None:
        self.offset = 0
        self.partial = ""
        self.line_number = 0

    def run(self) -> None:
        while not self.stop_event.is_set():
            try:
                stat = self.path.stat()
            except FileNotFoundError:
                self.stop_event.wait(self.poll_interval)
                continue

            if self.inode is None:
                self.inode = stat.st_ino
            elif self.inode != stat.st_ino or stat.st_size < self.offset:
                self.inode = stat.st_ino
                self._reset()
                self.broadcaster.publish(self.state.reset().as_message())

            if stat.st_size > self.offset:
                with self.path.open("r", encoding="utf-8") as handle:
                    handle.seek(self.offset)
                    chunk = handle.read()
                    self.offset = handle.tell()
                self._consume(chunk)

            self.stop_event.wait(self.poll_interval)

    def _consume(self, chunk: str) -> None:
        text = self.partial + chunk
        lines = text.split("\n")
        self.partial = lines.pop()
        for line in lines:
            self.line_number += 1
            stripped = line.strip()
            if not stripped:
                continue
            try:
                raw = json.loads(stripped)
                if not isinstance(raw, dict):
                    raise ValueError("event must be a JSON object")
                messages = self.state.process_event(raw, line_number=self.line_number)
                for message in messages:
                    self.broadcaster.publish(message)
            except Exception as exc:
                print(f"Skipping {self.path}:{self.line_number}: {exc}", file=sys.stderr)
                self.broadcaster.publish(self.state.record_parse_error())


class LiveServerContext:
    """Own the currently attached log and support safe runtime switching."""

    def __init__(
        self,
        *,
        broadcaster: SSEBroadcaster,
        watch_root: Path,
        output_dir: Path,
        output_name: str,
        inline_plotly: bool,
        poll_interval: float,
    ) -> None:
        self.broadcaster = broadcaster
        self.watch_root = watch_root.resolve()
        self.output_dir = output_dir.resolve()
        self.output_name = output_name
        self.inline_plotly = inline_plotly
        self.poll_interval = poll_interval
        self.export_lock = threading.Lock()
        self.switch_lock = threading.RLock()
        self.last_export_error: str | None = None
        self.state: LiveReconstructor | None = None
        self.source_path: Path | None = None
        self.output_html: Path | None = None
        self.tailer: JsonlTailer | None = None
        self.tailer_stop_event: threading.Event | None = None

    def resolve_source_path(self, value: str | Path) -> Path:
        text = str(value).strip()
        if not text:
            raise ValueError("Choose a JSONL path first")
        candidate = Path(text).expanduser()
        if not candidate.is_absolute():
            candidate = self.watch_root / candidate
        candidate = candidate.resolve(strict=False)
        try:
            candidate.relative_to(self.watch_root)
        except ValueError as exc:
            raise ValueError(
                f"The selected file must be inside the watch directory: {self.watch_root}"
            ) from exc
        if candidate.suffix.lower() != ".jsonl":
            raise ValueError("The selected file must have a .jsonl extension")
        return candidate

    def relative_source_path(self, source: Path | None = None) -> str | None:
        source = source or self.source_path
        if source is None:
            return None
        try:
            return source.relative_to(self.watch_root).as_posix()
        except ValueError:
            return source.name

    def output_path_for(self, source: Path) -> Path:
        relative = Path(self.relative_source_path(source) or source.name).with_suffix("")
        safe_parts = [re.sub(r"[^A-Za-z0-9._-]+", "_", part).strip("._") or "log" for part in relative.parts]
        return self.output_dir.joinpath(*safe_parts, self.output_name)

    def list_log_files(self, limit: int = 2000) -> list[dict[str, Any]]:
        if not self.watch_root.exists():
            return []
        files: list[tuple[int, Path]] = []
        for path in self.watch_root.rglob("*.jsonl"):
            try:
                stat = path.stat()
            except OSError:
                continue
            files.append((stat.st_mtime_ns, path))
        files.sort(key=lambda item: item[0], reverse=True)
        active = self.source_path
        result: list[dict[str, Any]] = []
        for modified_ns, path in files[: max(1, limit)]:
            try:
                stat = path.stat()
                relative = path.relative_to(self.watch_root).as_posix()
            except (OSError, ValueError):
                continue
            result.append({
                "path": relative,
                "sizeBytes": stat.st_size,
                "modifiedUnixNs": modified_ns,
                "active": active == path.resolve(strict=False),
            })
        return result

    def _empty_snapshot_message(self) -> dict[str, Any]:
        return {
            "type": "snapshot",
            "revision": 0,
            "attached": False,
            "sourcePath": None,
            "sourceRelativePath": None,
            "watchRoot": str(self.watch_root),
            "outputPath": None,
            "staticReportAvailable": False,
            "events": [],
            "completedSpans": [],
            "openSpans": [],
            "firstStartNs": None,
            "estimatedElapsedNs": 0,
            "unmatchedEndCount": 0,
            "parseErrorCount": 0,
        }

    def snapshot_message(self) -> dict[str, Any]:
        with self.switch_lock:
            if self.state is None or self.source_path is None:
                return self._empty_snapshot_message()
            message = self.state.snapshot().as_message()
            message.update({
                "attached": True,
                "sourceRelativePath": self.relative_source_path(),
                "watchRoot": str(self.watch_root),
                "outputPath": str(self.output_html) if self.output_html else None,
                "staticReportAvailable": bool(self.output_html and self.output_html.is_file()),
            })
            return message

    def heartbeat_message(self) -> dict[str, Any]:
        with self.switch_lock:
            if self.state is None:
                return {
                    "type": "clock_sync",
                    "revision": 0,
                    "attached": False,
                    "estimatedElapsedNs": 0,
                }
            snapshot = self.state.snapshot()
            return {
                "type": "clock_sync",
                "revision": snapshot.revision,
                "attached": True,
                "estimatedElapsedNs": self.state.estimated_elapsed_ns(),
            }

    def _stop_tailer_locked(self) -> None:
        if self.tailer_stop_event is not None:
            self.tailer_stop_event.set()
        if self.tailer is not None:
            self.tailer.join(timeout=max(2.0, self.poll_interval * 4))
        self.tailer = None
        self.tailer_stop_event = None

    def attach(self, value: str | Path) -> dict[str, Any]:
        source = self.resolve_source_path(value)
        with self.switch_lock:
            if self.source_path == source and self.state is not None:
                return {
                    "ok": True,
                    "changed": False,
                    "sourcePath": str(source),
                    "sourceRelativePath": self.relative_source_path(source),
                    "outputPath": str(self.output_html) if self.output_html else None,
                }

            previous_export: dict[str, Any] | None = None
            if self.state is not None and self.state.snapshot().completed_spans:
                try:
                    previous_export = self.export_static()
                except Exception as exc:  # best effort before switching
                    self.last_export_error = str(exc)

            self._stop_tailer_locked()
            self.source_path = source
            self.output_html = self.output_path_for(source)
            self.state = LiveReconstructor(source)
            self.tailer_stop_event = threading.Event()
            self.tailer = JsonlTailer(
                path=source,
                state=self.state,
                broadcaster=self.broadcaster,
                stop_event=self.tailer_stop_event,
                poll_interval=self.poll_interval,
            )
            self.tailer.start()
            snapshot = self.snapshot_message()
            snapshot["type"] = "snapshot"
            snapshot["attachmentChanged"] = True
            self.broadcaster.publish(snapshot)
            return {
                "ok": True,
                "changed": True,
                "sourcePath": str(source),
                "sourceRelativePath": self.relative_source_path(source),
                "waitingForFile": not source.exists(),
                "outputPath": str(self.output_html),
                "previousExport": previous_export,
            }

    def detach(self, *, export_current: bool = True) -> dict[str, Any]:
        with self.switch_lock:
            previous_source = self.source_path
            exported: dict[str, Any] | None = None
            if export_current and self.state is not None and self.state.snapshot().completed_spans:
                try:
                    exported = self.export_static()
                except Exception as exc:
                    self.last_export_error = str(exc)
            self._stop_tailer_locked()
            self.state = None
            self.source_path = None
            self.output_html = None
            snapshot = self._empty_snapshot_message()
            snapshot["attachmentChanged"] = True
            self.broadcaster.publish(snapshot)
            return {
                "ok": True,
                "detached": str(previous_source) if previous_source else None,
                "exported": exported,
            }

    def export_static(self) -> dict[str, Any]:
        with self.switch_lock, self.export_lock:
            if self.state is None or self.source_path is None or self.output_html is None:
                raise ValueError("No log file is attached")
            snapshot = self.state.snapshot()
            if not snapshot.completed_spans:
                raise ValueError("No completed spans are available yet")

            run = snapshot_to_static_run(snapshot, self.output_html)
            self.output_html.parent.mkdir(parents=True, exist_ok=True)
            spans_dir = self.output_html.parent / "spans"
            spans_dir.mkdir(parents=True, exist_ok=True)
            spans_csv = spans_dir / f"{run['safe_id']}.spans.csv"
            run["spans"].drop(
                columns=["start_event", "end_event", "attributes"],
                errors="ignore",
            ).to_csv(spans_csv, index=False)
            run["spans_csv"] = spans_csv
            build_dashboard(
                runs=[run],
                output_html=self.output_html,
                inline_plotly=self.inline_plotly,
                average_index=None,
            )
            self.state.mark_exported()
            self.last_export_error = None
            result = {
                "ok": True,
                "path": str(self.output_html),
                "url": "/static-report",
                "sourcePath": str(self.source_path),
                "completedSpans": len(snapshot.completed_spans),
                "openSpans": len(snapshot.open_spans),
            }
            self.broadcaster.publish({"type": "static_exported", **result})
            return result

    def should_idle_export(self, idle_seconds: float) -> bool:
        with self.switch_lock:
            return bool(self.state and self.state.should_idle_export(idle_seconds))

    def shutdown(self) -> None:
        self.detach(export_current=True)

def snapshot_to_static_run(snapshot: LiveStateSnapshot, output_html: Path) -> dict[str, Any]:
    completed = snapshot.completed_spans
    if not completed:
        raise ValueError("No completed spans are available")
    t0 = min(int(span["startNs"]) for span in completed)
    rows: list[dict[str, Any]] = []
    for item in completed:
        start_ns = int(item["startNs"])
        end_ns = int(item["endNs"])
        duration_ns = end_ns - start_ns
        attributes = copy.deepcopy(item.get("attributes") or {})
        row = {
            "name": item["name"],
            "thread_id": item["threadId"],
            "thread_index": int(item["threadIndex"]),
            "seq_start": item.get("seqStart"),
            "seq_end": item.get("seqEnd"),
            "start_ns": start_ns,
            "end_ns": end_ns,
            "duration_ns": duration_ns,
            "duration_us_calc": duration_ns / 1_000.0,
            "duration_ms": duration_ns / 1_000_000.0,
            "duration_us_logged": None,
            "depth": attributes.get("depth"),
            "row_size": attributes.get("row_size"),
            "frontier_size": attributes.get("frontier_size"),
            "attributes": attributes,
            "attributes_json": json.dumps(
                attributes,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            ),
            "start_event": {},
            "end_event": {},
            "start_ms": (start_ns - t0) / 1_000_000.0,
            "end_ms": (end_ns - t0) / 1_000_000.0,
        }
        for key, value in attributes.items():
            row[f"attr.{key}"] = value
        rows.append(row)

    spans = pd.DataFrame(rows).sort_values(["start_ns", "end_ns"]).reset_index(drop=True)
    spans.attrs["unclosed_count"] = len(snapshot.open_spans)
    spans.attrs["unmatched_end_count"] = snapshot.unmatched_end_count
    events = pd.DataFrame(snapshot.events)
    source = Path(snapshot.source_path)
    safe_id = make_safe_html_id(source.stem or "live_run")
    return {
        "id": safe_id,
        "safe_id": safe_id,
        "label": source.name,
        "relative_path": source,
        "log_file": source,
        "events": events,
        "spans": spans,
        "spans_csv": output_html.parent / "spans" / f"{safe_id}.spans.csv",
        "is_average": False,
    }


def _json_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def make_handler(context: LiveServerContext):
    class Handler(BaseHTTPRequestHandler):
        server_version = "POLYGRAPHLiveTimeline/2.0"

        def log_message(self, fmt: str, *args: Any) -> None:
            print(f"[{self.log_date_time_string()}] {fmt % args}")

        def _send_bytes(self, status: int, content_type: str, payload: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)

        def _send_json(self, status: int, value: Any) -> None:
            self._send_bytes(status, "application/json; charset=utf-8", _json_bytes(value))

        def _read_json_body(self) -> dict[str, Any]:
            length = int(self.headers.get("Content-Length", "0") or 0)
            if length <= 0:
                return {}
            value = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(value, dict):
                raise ValueError("Request body must be a JSON object")
            return value

        def do_GET(self) -> None:  # noqa: N802
            path = urlparse(self.path).path
            if path == "/":
                payload = build_live_dashboard_html(
                    watch_root=context.watch_root,
                    inline_plotly=context.inline_plotly,
                ).encode("utf-8")
                self._send_bytes(HTTPStatus.OK, "text/html; charset=utf-8", payload)
                return
            if path == "/events":
                self._serve_events()
                return
            if path == "/api/snapshot":
                self._send_json(HTTPStatus.OK, context.snapshot_message())
                return
            if path == "/api/files":
                self._send_json(HTTPStatus.OK, {
                    "watchRoot": str(context.watch_root),
                    "files": context.list_log_files(),
                })
                return
            if path == "/static-report":
                output_html = context.output_html
                if output_html is None or not output_html.is_file():
                    self._send_json(HTTPStatus.NOT_FOUND, {"error": "Static report has not been saved for the attached log"})
                    return
                self._send_bytes(
                    HTTPStatus.OK,
                    "text/html; charset=utf-8",
                    output_html.read_bytes(),
                )
                return
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "Not found"})

        def do_POST(self) -> None:  # noqa: N802
            path = urlparse(self.path).path
            try:
                if path == "/api/export":
                    result = context.export_static()
                elif path == "/api/attach":
                    body = self._read_json_body()
                    result = context.attach(body.get("path", ""))
                elif path == "/api/detach":
                    result = context.detach(export_current=True)
                else:
                    self._send_json(HTTPStatus.NOT_FOUND, {"error": "Not found"})
                    return
                self._send_json(HTTPStatus.OK, result)
            except Exception as exc:
                context.last_export_error = str(exc)
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})

        def _serve_events(self) -> None:
            client = context.broadcaster.register()
            try:
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/event-stream; charset=utf-8")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.send_header("X-Accel-Buffering", "no")
                self.end_headers()
                self._write_sse(context.snapshot_message())
                last_heartbeat = time.monotonic()
                while True:
                    try:
                        message = client.get(timeout=1.0)
                        self._write_sse(message)
                    except queue.Empty:
                        now = time.monotonic()
                        if now - last_heartbeat >= 1.0:
                            self._write_sse(context.heartbeat_message())
                            last_heartbeat = now
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            finally:
                context.broadcaster.unregister(client)

        def _write_sse(self, message: dict[str, Any]) -> None:
            payload = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
            self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
            self.wfile.flush()

    return Handler

def build_live_dashboard_html(watch_root: Path, inline_plotly: bool) -> str:
    if inline_plotly:
        from plotly.offline import get_plotlyjs
        plotly_script = f"<script>{get_plotlyjs()}</script>"
    else:
        plotly_script = '<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>'

    document = LIVE_DASHBOARD_TEMPLATE
    document = document.replace("__PLOTLY_SCRIPT__", plotly_script)
    document = document.replace("__WATCH_ROOT__", html.escape(str(watch_root)))
    document = document.replace("__WATCH_ROOT_JSON__", json.dumps(str(watch_root)))
    return document


LIVE_DASHBOARD_TEMPLATE = r'''<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Live C++ Event Timeline</title>
  __PLOTLY_SCRIPT__
  <style>
    :root { color-scheme: light; font-family: Inter, ui-sans-serif, system-ui, sans-serif; }
    * { box-sizing: border-box; }
    body { margin: 0; background: #f5f7fb; color: #172033; }
    header { position: sticky; top: 0; z-index: 5; padding: 18px 24px; background: rgba(255,255,255,.96); border-bottom: 1px solid #dfe5ef; backdrop-filter: blur(8px); }
    h1 { margin: 0 0 12px; font-size: 24px; }
    .controls { display: flex; flex-wrap: wrap; gap: 12px; align-items: end; }
    label { display: grid; gap: 5px; font-size: 12px; font-weight: 700; color: #5a6578; }
    select, input, button, a.button { font: inherit; }
    select, input[type="number"], input[type="text"] { min-height: 36px; border: 1px solid #cfd7e5; border-radius: 8px; background: white; padding: 6px 9px; }
    button, a.button { min-height: 36px; border: 0; border-radius: 8px; padding: 8px 13px; background: #253b80; color: white; cursor: pointer; text-decoration: none; font-weight: 700; }
    button.secondary, a.secondary { background: #e9edf6; color: #26324b; }
    button:disabled { opacity: .5; cursor: default; }
    .connection { margin-left: auto; display: flex; gap: 8px; align-items: center; font-size: 13px; }
    .dot { width: 10px; height: 10px; border-radius: 50%; background: #d08520; }
    .dot.connected { background: #2d9b55; }
    .dot.error { background: #bf3b3b; }
    .source { margin-top: 10px; color: #687388; font-size: 12px; overflow-wrap: anywhere; }
    .attachment-bar { display: grid; grid-template-columns: minmax(260px, 1fr) auto minmax(300px, 1.4fr) auto auto; gap: 8px; align-items: end; margin-bottom: 12px; padding: 12px; border: 1px solid #dfe5ef; border-radius: 10px; background: #f8faff; }
    .attachment-bar select, .attachment-bar input { width: 100%; }
    .attachment-message { grid-column: 1 / -1; min-height: 18px; color: #566279; font-size: 12px; }
    .waiting { color: #a65f00; }
    .success { color: #287849; }
    main { padding: 20px 24px 44px; }
    .panel { display: none; }
    .panel.active { display: block; }
    .card { background: white; border: 1px solid #dfe5ef; border-radius: 12px; box-shadow: 0 4px 18px rgba(35,49,80,.06); padding: 18px; margin-bottom: 16px; }
    .metric-grid { display: grid; grid-template-columns: repeat(auto-fit,minmax(150px,1fr)); gap: 12px; }
    .metric { padding: 14px; border-radius: 10px; background: #f6f8fc; }
    .metric span { display: block; color: #6b7688; font-size: 12px; }
    .metric strong { display: block; margin-top: 4px; font-size: 22px; }
    .filter-heading { display: flex; flex-wrap: wrap; gap: 10px; justify-content: space-between; align-items: center; margin-bottom: 10px; }
    .filter-heading span { display: block; color: #737d8f; font-size: 12px; font-weight: 400; }
    .chip-list { display: flex; flex-wrap: wrap; gap: 7px; }
    .chip { display: flex; grid-template-columns: none; flex-direction: row; align-items: center; gap: 6px; border: 1px solid #d5ddea; background: #f8faff; border-radius: 999px; padding: 6px 9px; color: #33405b; font-weight: 600; }
    .chip small { color: #7b8597; }
    .toolbar { display: flex; flex-wrap: wrap; gap: 8px; align-items: end; }
    .filter-cards { display: grid; gap: 8px; margin-top: 10px; }
    .filter-card { padding: 11px; border: 1px solid #d9e0eb; border-radius: 9px; background: #fbfcff; }
    .filter-card-header { display: flex; justify-content: space-between; gap: 8px; align-items: center; margin-bottom: 8px; }
    .filter-card-header button { min-height: 28px; padding: 4px 8px; background: #e9edf6; color: #26324b; }
    .status { margin-top: 10px; color: #58647a; font-size: 13px; }
    #timeline { min-height: 560px; }
    .export-message { color: #566279; font-size: 13px; }
    .warning { color: #a65f00; }
    @media (max-width: 900px) { .attachment-bar { grid-template-columns: 1fr auto; } .attachment-bar .path-field { grid-column: 1 / -1; } }
    @media (max-width: 700px) { header, main { padding-left: 12px; padding-right: 12px; } .connection { width: 100%; margin-left: 0; } .attachment-bar { grid-template-columns: 1fr; } .attachment-bar .path-field { grid-column: auto; } }
  </style>
</head>
<body>
<header>
  <h1>Live C++ Event Timeline</h1>
  <div class="attachment-bar">
    <label>Available JSONL logs
      <select id="logFileSelect"><option value="">Loading files…</option></select>
    </label>
    <button id="refreshFiles" class="secondary" type="button">Refresh</button>
    <label class="path-field">Path under watch directory
      <input id="logPathInput" type="text" placeholder="run-id/events/start-uuid/1.events.jsonl" />
    </label>
    <button id="attachButton" type="button">Attach</button>
    <button id="detachButton" class="secondary" type="button" disabled>Detach</button>
    <div id="attachmentMessage" class="attachment-message">Choose an existing log or enter the path of a file that will be created later.</div>
  </div>
  <div class="controls">
    <label>View
      <select id="viewSelect"><option value="overview">Overview</option><option value="timeline">Timeline: by thread</option></select>
    </label>
    <button id="saveButton" type="button" disabled>Save static HTML</button>
    <a id="openStatic" class="button secondary" href="/static-report" target="_blank" hidden>Open saved HTML</a>
    <span id="exportMessage" class="export-message"></span>
    <div class="connection"><span id="connectionDot" class="dot"></span><span id="connectionText">Connecting…</span></div>
  </div>
  <div id="sourceStatus" class="source">Watch directory: __WATCH_ROOT__ · No log attached.</div>
</header>
<main>
  <section id="overviewPanel" class="panel active">
    <div class="card">
      <div class="metric-grid">
        <div class="metric"><span>Events read</span><strong id="eventCount">0</strong></div>
        <div class="metric"><span>Completed spans</span><strong id="completedCount">0</strong></div>
        <div class="metric"><span>Running spans</span><strong id="runningCount">0</strong></div>
        <div class="metric"><span>Threads</span><strong id="threadCount">0</strong></div>
        <div class="metric"><span>Visible window</span><strong id="windowMs">0 ms</strong></div>
        <div class="metric"><span>Input errors</span><strong id="errorCount">0</strong></div>
      </div>
    </div>
    <div class="card"><h2>Top event types by completed time</h2><div id="topEvents">No completed spans yet.</div></div>
  </section>
  <section id="timelinePanel" class="panel">
    <div class="card">
      <div class="filter-heading"><div><strong>Event families</strong><span>Toggle every event sharing its first name component.</span></div><div><button class="secondary" id="showAll">Show all</button> <button class="secondary" id="hideAll">Hide all</button></div></div>
      <div id="familyFilters" class="chip-list"></div>
    </div>
    <div class="card">
      <div class="filter-heading"><div><strong>Event attributes</strong><span>Values within a card use OR; multiple cards combine with AND.</span></div><button class="secondary" id="resetFilters">Reset filters</button></div>
      <div class="toolbar"><label>Add attribute<select id="attributeSelect"><option value="">Choose an attribute…</option></select></label><button id="addAttribute" disabled>Add filter</button></div>
      <div id="attributeCards" class="filter-cards"></div>
    </div>
    <div class="card">
      <div class="filter-heading"><div><strong>Event duration ranges</strong><span>Running spans enter or leave the range while their bars grow.</span></div></div>
      <div class="toolbar"><label>Add event<select id="durationSelect"><option value="">Choose an event type…</option></select></label><button id="addDuration" disabled>Add range</button></div>
      <div id="durationCards" class="filter-cards"></div>
      <div id="filterStatus" class="status"></div>
    </div>
    <div class="card"><div id="timeline"></div></div>
  </section>
</main>
<script>
(() => {
  const watchRoot = __WATCH_ROOT_JSON__;
  let currentSourcePath = null;
  let currentSourceRelativePath = null;
  let attached = false;
  const spans = new Map();
  const eventFamilies = new Map();
  const activeFamilies = new Set();
  const activeAttributeFilters = new Map();
  const activeDurationFilters = new Map();
  let eventCount = 0;
  let unmatchedEndCount = 0;
  let parseErrorCount = 0;
  let firstStartNs = null;
  let syncElapsedNs = 0;
  let syncReceivedAtMs = performance.now();
  let lastRevision = -1;
  let renderPending = false;
  let lastRenderAtMs = 0;
  let filterOptionsDirty = true;

  const plotConfig = {responsive: true, displaylogo: false};
  const timeline = document.getElementById('timeline');

  function estimatedElapsedNs() {
    return syncElapsedNs + Math.max(0, performance.now() - syncReceivedAtMs) * 1e6;
  }
  function applySync(value) {
    if (Number.isFinite(Number(value))) {
      syncElapsedNs = Number(value);
      syncReceivedAtMs = performance.now();
    }
  }
  function familyOf(name) { return String(name).split('.', 1)[0]; }
  function valueToken(value) {
    if (typeof value === 'boolean') return `boolean:${value}`;
    if (typeof value === 'number') return `number:${JSON.stringify(value)}`;
    if (typeof value === 'string') return `string:${value}`;
    return `json:${JSON.stringify(value)}`;
  }
  function valueLabel(value) {
    if (value === null) return 'null';
    if (typeof value === 'boolean') return value ? 'True' : 'False';
    if (typeof value === 'object') return JSON.stringify(value);
    return String(value);
  }
  function escapeHtml(value) {
    return String(value).replace(/[&<>'"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]));
  }
  function durationNs(span) {
    return span.running ? Math.max(0, estimatedElapsedNs() - span.startNs) : Math.max(0, span.endNs - span.startNs);
  }
  function laneName(span) { return `thread ${span.threadIndex} / lane ${span.lane}`; }

  function updateAttachmentUi(message) {
    attached = Boolean(message.attached);
    currentSourcePath = message.sourcePath || null;
    currentSourceRelativePath = message.sourceRelativePath || null;
    const sourceStatus = document.getElementById('sourceStatus');
    sourceStatus.textContent = attached
      ? `Watch directory: ${watchRoot} · Attached: ${currentSourceRelativePath || currentSourcePath}`
      : `Watch directory: ${watchRoot} · No log attached.`;
    document.getElementById('saveButton').disabled = !attached;
    document.getElementById('detachButton').disabled = !attached;
    document.getElementById('openStatic').hidden = !Boolean(message.staticReportAvailable);
    if (attached && currentSourceRelativePath) {
      document.getElementById('logPathInput').value = currentSourceRelativePath;
    }
  }

  function replaceState(message) {
    const sourceChanged = currentSourcePath !== (message.sourcePath || null);
    if (sourceChanged) {
      activeFamilies.clear();
      activeAttributeFilters.clear();
      activeDurationFilters.clear();
      document.getElementById('exportMessage').textContent = '';
    }
    updateAttachmentUi(message);
    spans.clear();
    for (const span of [...(message.completedSpans || []), ...(message.openSpans || [])]) spans.set(span.id, span);
    eventCount = (message.events || []).length;
    unmatchedEndCount = Number(message.unmatchedEndCount || 0);
    parseErrorCount = Number(message.parseErrorCount || 0);
    firstStartNs = message.firstStartNs;
    applySync(message.estimatedElapsedNs);
    lastRevision = Number(message.revision || 0);
    filterOptionsDirty = true;
    scheduleRender();
    refreshFiles(true);
  }

  function handleMessage(message) {
    if (message.type === 'snapshot') { replaceState(message); return; }
    const revision = Number(message.revision ?? -1);
    if (revision >= 0 && revision <= lastRevision && message.type !== 'clock_sync') return;
    if (revision >= 0) lastRevision = revision;
    applySync(message.estimatedElapsedNs);
    if (message.type === 'span_opened' || message.type === 'span_completed') {
      spans.set(message.span.id, message.span);
      eventCount += 1;
      if (firstStartNs === null || message.span.startNs < firstStartNs) firstStartNs = message.span.startNs;
      filterOptionsDirty = true;
    } else if (message.type === 'event') {
      eventCount += 1;
    } else if (message.type === 'unmatched_end') {
      eventCount += 1;
      unmatchedEndCount = Number(message.unmatchedEndCount || unmatchedEndCount + 1);
    } else if (message.type === 'parse_error') {
      parseErrorCount = Number(message.parseErrorCount || parseErrorCount + 1);
    } else if (message.type === 'static_exported') {
      document.getElementById('openStatic').hidden = false;
      document.getElementById('exportMessage').textContent = `Saved ${message.completedSpans} spans to ${message.path}.`;
    }
    scheduleRender();
  }

  function passesFilters(span) {
    if (!activeFamilies.has(familyOf(span.name))) return false;
    for (const [attribute, selected] of activeAttributeFilters) {
      const attrs = span.attributes || {};
      const token = Object.prototype.hasOwnProperty.call(attrs, attribute) ? valueToken(attrs[attribute]) : '__missing__';
      if (selected.size && !selected.has(token)) return false;
    }
    const range = activeDurationFilters.get(span.name);
    if (range) {
      const ms = durationNs(span) / 1e6;
      if (range.min !== null && ms < range.min) return false;
      if (range.max !== null && ms > range.max) return false;
    }
    return true;
  }

  function hoverText(span) {
    const durationMs = durationNs(span) / 1e6;
    const startMs = firstStartNs === null ? 0 : (span.startNs - firstStartNs) / 1e6;
    const endLine = span.running ? '<b>status: running</b>' : `end: ${((span.endNs - firstStartNs) / 1e6).toFixed(3)} ms`;
    const attrs = Object.entries(span.attributes || {}).sort((a,b) => a[0].localeCompare(b[0])).map(([k,v]) => `${escapeHtml(k)}: ${escapeHtml(valueLabel(v))}`);
    return [`<b>${escapeHtml(span.name)}</b>`, `thread: ${span.threadIndex}`, `lane: ${span.lane}`, `start: ${startMs.toFixed(3)} ms`, endLine, `duration: ${durationMs.toFixed(3)} ms`, `seq: ${span.seqStart}${span.seqEnd == null ? '' : ' → ' + span.seqEnd}`, ...attrs].join('<br>');
  }

  function rebuildFilterOptions() {
    const familyCounts = new Map();
    const attributes = new Map();
    const events = new Set();
    for (const span of spans.values()) {
      const family = familyOf(span.name);
      familyCounts.set(family, (familyCounts.get(family) || 0) + 1);
      events.add(span.name);
      for (const [key, value] of Object.entries(span.attributes || {})) {
        if (!attributes.has(key)) attributes.set(key, new Map());
        const values = attributes.get(key);
        const token = valueToken(value);
        if (!values.has(token)) values.set(token, {token, label: valueLabel(value), count: 0});
        values.get(token).count += 1;
      }
    }
    for (const family of familyCounts.keys()) if (!eventFamilies.has(family)) activeFamilies.add(family);
    eventFamilies.clear();
    for (const [family, count] of familyCounts) eventFamilies.set(family, count);
    const familyContainer = document.getElementById('familyFilters');
    familyContainer.innerHTML = '';
    for (const [family, count] of [...eventFamilies.entries()].sort()) {
      const label = document.createElement('label'); label.className = 'chip';
      label.innerHTML = `<input type="checkbox" ${activeFamilies.has(family) ? 'checked' : ''}><span>${escapeHtml(family)}</span><small>${count}</small>`;
      label.querySelector('input').addEventListener('change', e => { e.target.checked ? activeFamilies.add(family) : activeFamilies.delete(family); scheduleRender(); });
      familyContainer.appendChild(label);
    }
    const attributeSelect = document.getElementById('attributeSelect');
    const currentAttribute = attributeSelect.value;
    attributeSelect.innerHTML = '<option value="">Choose an attribute…</option>';
    for (const key of [...attributes.keys()].sort()) if (!activeAttributeFilters.has(key)) attributeSelect.add(new Option(key, key));
    attributeSelect.value = [...attributeSelect.options].some(o => o.value === currentAttribute) ? currentAttribute : '';
    document.getElementById('addAttribute').disabled = !attributeSelect.value;
    const durationSelect = document.getElementById('durationSelect');
    const currentEvent = durationSelect.value;
    durationSelect.innerHTML = '<option value="">Choose an event type…</option>';
    for (const name of [...events].sort()) if (!activeDurationFilters.has(name)) durationSelect.add(new Option(name, name));
    durationSelect.value = [...durationSelect.options].some(o => o.value === currentEvent) ? currentEvent : '';
    document.getElementById('addDuration').disabled = !durationSelect.value;
    window.__liveAttributes = attributes;
    filterOptionsDirty = false;
  }

  function renderAttributeCards() {
    const container = document.getElementById('attributeCards'); container.innerHTML = '';
    const attributes = window.__liveAttributes || new Map();
    for (const [key, selected] of activeAttributeFilters) {
      const card = document.createElement('div'); card.className = 'filter-card';
      card.innerHTML = `<div class="filter-card-header"><strong>${escapeHtml(key)}</strong><button type="button">Remove</button></div><div class="chip-list"></div>`;
      card.querySelector('button').addEventListener('click', () => { activeAttributeFilters.delete(key); filterOptionsDirty = true; scheduleRender(); });
      const values = attributes.get(key) || new Map();
      let present = 0;
      for (const span of spans.values()) if (Object.prototype.hasOwnProperty.call(span.attributes || {}, key)) present += 1;
      const options = [...values.values(), {token:'__missing__', label:'Missing', count:spans.size-present}].filter(x => x.count > 0);
      for (const option of options.sort((a,b) => a.label.localeCompare(b.label))) {
        if (selected.size === 0) selected.add(option.token);
        const label = document.createElement('label'); label.className = 'chip';
        label.innerHTML = `<input type="checkbox" ${selected.has(option.token) ? 'checked' : ''}><span>${escapeHtml(option.label)}</span><small>${option.count}</small>`;
        label.querySelector('input').addEventListener('change', e => { e.target.checked ? selected.add(option.token) : selected.delete(option.token); scheduleRender(); });
        card.querySelector('.chip-list').appendChild(label);
      }
      container.appendChild(card);
    }
  }

  function renderDurationCards() {
    const container = document.getElementById('durationCards'); container.innerHTML = '';
    for (const [name, range] of activeDurationFilters) {
      const card = document.createElement('div'); card.className = 'filter-card';
      card.innerHTML = `<div class="filter-card-header"><strong>${escapeHtml(name)}</strong><button type="button">Remove</button></div><div class="toolbar"><label>Minimum ms<input data-min type="number" step="any" placeholder="No minimum"></label><label>Maximum ms<input data-max type="number" step="any" placeholder="No maximum"></label></div>`;
      card.querySelector('button').addEventListener('click', () => { activeDurationFilters.delete(name); filterOptionsDirty = true; scheduleRender(); });
      const minInput = card.querySelector('[data-min]'); const maxInput = card.querySelector('[data-max]');
      minInput.value = range.min ?? ''; maxInput.value = range.max ?? '';
      const update = () => { range.min = minInput.value === '' ? null : Number(minInput.value); range.max = maxInput.value === '' ? null : Number(maxInput.value); scheduleRender(); };
      minInput.addEventListener('input', update); maxInput.addEventListener('input', update);
      container.appendChild(card);
    }
  }

  function renderOverview(allSpans) {
    const completed = allSpans.filter(s => !s.running); const running = allSpans.filter(s => s.running);
    document.getElementById('eventCount').textContent = String(eventCount);
    document.getElementById('completedCount').textContent = String(completed.length);
    document.getElementById('runningCount').textContent = String(running.length);
    document.getElementById('threadCount').textContent = String(new Set(allSpans.map(s => s.threadId)).size);
    document.getElementById('errorCount').textContent = String(unmatchedEndCount + parseErrorCount);
    const windowEndNs = running.length ? estimatedElapsedNs() : (completed.length ? Math.max(...completed.map(s => s.endNs)) : firstStartNs);
    const windowMs = firstStartNs === null || windowEndNs === null ? 0 : Math.max(0, windowEndNs - firstStartNs) / 1e6;
    document.getElementById('windowMs').textContent = `${windowMs.toFixed(1)} ms`;
    const totals = new Map();
    for (const span of completed) totals.set(span.name, (totals.get(span.name) || 0) + durationNs(span) / 1e6);
    const rows = [...totals.entries()].sort((a,b) => b[1]-a[1]).slice(0,10);
    document.getElementById('topEvents').innerHTML = rows.length ? `<table><tbody>${rows.map(([n,v]) => `<tr><td>${escapeHtml(n)}</td><td>${v.toFixed(3)} ms</td></tr>`).join('')}</tbody></table>` : 'No completed spans yet.';
  }

  function render() {
    renderPending = false;
    lastRenderAtMs = performance.now();
    const allSpans = [...spans.values()];
    renderOverview(allSpans);
    if (document.getElementById('viewSelect').value !== 'timeline') return;
    if (filterOptionsDirty) { rebuildFilterOptions(); renderAttributeCards(); renderDurationCards(); }
    const visible = allSpans.filter(passesFilters);
    const grouped = new Map();
    for (const span of visible) { if (!grouped.has(span.name)) grouped.set(span.name, []); grouped.get(span.name).push(span); }
    const data = [];
    const lanes = [];
    for (const [name, group] of [...grouped.entries()].sort((a,b) => a[0].localeCompare(b[0]))) {
      group.sort((a,b) => a.startNs-b.startNs);
      for (const span of group) if (!lanes.includes(laneName(span))) lanes.push(laneName(span));
      data.push({
        type:'bar', orientation:'h', name,
        x:group.map(s => durationNs(s)/1e6),
        base:group.map(s => firstStartNs === null ? 0 : (s.startNs-firstStartNs)/1e6),
        y:group.map(laneName),
        customdata:group.map(s => [hoverText(s), s.running]),
        hovertemplate:'%{customdata[0]}<extra></extra>',
        marker:{line:{width:group.map(s => s.running ? 2 : 0), color:'rgba(20,30,45,.65)'}},
        opacity:0.92
      });
    }
    const orderedLanes = [...lanes].reverse();
    const layout = {title:'Execution Timeline by Thread — live', xaxis:{title:'Time since first span [ms]'}, yaxis:{title:'Execution lane',categoryorder:'array',categoryarray:orderedLanes}, barmode:'overlay',bargap:.20,height:Math.max(550,80+42*orderedLanes.length),legend:{title:{text:'Event type'}},margin:{l:280,r:40,t:80,b:60},uirevision:'live-timeline'};
    Plotly.react(timeline,data,layout,plotConfig);
    document.getElementById('filterStatus').textContent = `${visible.length} of ${allSpans.length} spans visible across ${grouped.size} event types and ${orderedLanes.length} lanes`;
  }

  function scheduleRender() {
    if (renderPending) return;
    renderPending = true;
    const delay = Math.max(0, 100 - (performance.now() - lastRenderAtMs));
    setTimeout(() => requestAnimationFrame(render), delay);
  }

  async function postJson(url, body = {}) {
    const response = await fetch(url, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(body)
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || `Request failed: ${response.status}`);
    return result;
  }

  async function refreshFiles(quiet = false) {
    const select = document.getElementById('logFileSelect');
    const previous = select.value || currentSourceRelativePath || '';
    try {
      const response = await fetch('/api/files', {cache: 'no-store'});
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || 'Could not list files');
      select.textContent = '';
      const placeholder = document.createElement('option');
      placeholder.value = '';
      placeholder.textContent = result.files.length ? 'Choose a log…' : 'No JSONL files found yet';
      select.appendChild(placeholder);
      for (const file of result.files) {
        const option = document.createElement('option');
        option.value = file.path;
        const size = file.sizeBytes < 1024 ? `${file.sizeBytes} B` : `${(file.sizeBytes / 1024).toFixed(1)} KiB`;
        option.textContent = `${file.active ? '● ' : ''}${file.path} (${size})`;
        select.appendChild(option);
      }
      if ([...select.options].some(option => option.value === previous)) select.value = previous;
    } catch (error) {
      if (!quiet) document.getElementById('attachmentMessage').textContent = error.message;
    }
  }

  async function attachPath() {
    const path = document.getElementById('logPathInput').value.trim() || document.getElementById('logFileSelect').value;
    const message = document.getElementById('attachmentMessage');
    if (!path) { message.textContent = 'Choose a JSONL log or enter a future relative path.'; message.className = 'attachment-message warning'; return; }
    document.getElementById('attachButton').disabled = true;
    message.textContent = `Attaching ${path}…`;
    message.className = 'attachment-message';
    try {
      const result = await postJson('/api/attach', {path});
      const previous = result.previousExport ? ` Previous run saved to ${result.previousExport.path}.` : '';
      message.textContent = result.waitingForFile
        ? `Attached. Waiting for ${result.sourceRelativePath} to be created.${previous}`
        : `Attached to ${result.sourceRelativePath}.${previous}`;
      message.className = `attachment-message ${result.waitingForFile ? 'waiting' : 'success'}`;
      await refreshFiles(true);
    } catch (error) {
      message.textContent = error.message;
      message.className = 'attachment-message warning';
    } finally {
      document.getElementById('attachButton').disabled = false;
    }
  }

  document.getElementById('logFileSelect').addEventListener('change', event => {
    if (event.target.value) document.getElementById('logPathInput').value = event.target.value;
  });
  document.getElementById('refreshFiles').addEventListener('click', () => refreshFiles(false));
  document.getElementById('attachButton').addEventListener('click', attachPath);
  document.getElementById('logPathInput').addEventListener('keydown', event => { if (event.key === 'Enter') attachPath(); });
  document.getElementById('detachButton').addEventListener('click', async () => {
    const message = document.getElementById('attachmentMessage');
    try {
      const result = await postJson('/api/detach');
      message.textContent = result.exported ? `Detached. Final report saved to ${result.exported.path}.` : 'Detached.';
      message.className = 'attachment-message success';
    } catch (error) {
      message.textContent = error.message;
      message.className = 'attachment-message warning';
    }
  });

  document.getElementById('viewSelect').addEventListener('change', e => {
    document.getElementById('overviewPanel').classList.toggle('active', e.target.value === 'overview');
    document.getElementById('timelinePanel').classList.toggle('active', e.target.value === 'timeline');
    scheduleRender();
    setTimeout(() => window.dispatchEvent(new Event('resize')), 30);
  });
  document.getElementById('showAll').addEventListener('click', () => { for (const family of eventFamilies.keys()) activeFamilies.add(family); filterOptionsDirty = true; scheduleRender(); });
  document.getElementById('hideAll').addEventListener('click', () => { activeFamilies.clear(); filterOptionsDirty = true; scheduleRender(); });
  document.getElementById('resetFilters').addEventListener('click', () => { activeAttributeFilters.clear(); activeDurationFilters.clear(); activeFamilies.clear(); for (const family of eventFamilies.keys()) activeFamilies.add(family); filterOptionsDirty = true; scheduleRender(); });
  document.getElementById('attributeSelect').addEventListener('change', e => document.getElementById('addAttribute').disabled = !e.target.value);
  document.getElementById('addAttribute').addEventListener('click', () => { const select=document.getElementById('attributeSelect'); if (!select.value) return; activeAttributeFilters.set(select.value,new Set()); select.value=''; filterOptionsDirty=true; scheduleRender(); });
  document.getElementById('durationSelect').addEventListener('change', e => document.getElementById('addDuration').disabled = !e.target.value);
  document.getElementById('addDuration').addEventListener('click', () => { const select=document.getElementById('durationSelect'); if (!select.value) return; activeDurationFilters.set(select.value,{min:null,max:null}); select.value=''; filterOptionsDirty=true; scheduleRender(); });
  document.getElementById('saveButton').addEventListener('click', async () => {
    const button=document.getElementById('saveButton'); const message=document.getElementById('exportMessage'); button.disabled=true; message.textContent='Saving…';
    try { const response=await fetch('/api/export',{method:'POST'}); const result=await response.json(); if (!response.ok) throw new Error(result.error || 'Export failed'); message.textContent=`Saved ${result.completedSpans} spans${result.openSpans ? `; ${result.openSpans} still running` : ''}.`; document.getElementById('openStatic').hidden=false; }
    catch (error) { message.textContent=error.message; message.classList.add('warning'); }
    finally { button.disabled=false; }
  });

  const connectionDot=document.getElementById('connectionDot'); const connectionText=document.getElementById('connectionText');
  const stream=new EventSource('/events');
  stream.onopen=()=>{connectionDot.className='dot connected';connectionText.textContent='Live';};
  stream.onerror=()=>{connectionDot.className='dot error';connectionText.textContent='Reconnecting…';};
  stream.onmessage=event=>{try{handleMessage(JSON.parse(event.data));}catch(error){console.error(error);}};
  setInterval(() => { if ([...spans.values()].some(s => s.running)) scheduleRender(); }, 100);
  setInterval(() => refreshFiles(true), 2000);
  refreshFiles(true);
  scheduleRender();
})();
</script>
</body>
</html>'''


def run_idle_exporter(
    context: LiveServerContext,
    stop_event: threading.Event,
    idle_seconds: float,
) -> None:
    while not stop_event.wait(0.25):
        if not context.should_idle_export(idle_seconds):
            continue
        try:
            result = context.export_static()
            print(
                f"Auto-saved static report: {result['path']} "
                f"({result['completedSpans']} spans)"
            )
        except Exception as exc:
            context.last_export_error = str(exc)
            print(f"Static export failed: {exc}", file=sys.stderr)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Serve a live Timeline: by thread dashboard and attach JSONL logs from the UI."
    )
    parser.add_argument(
        "log_file",
        nargs="?",
        type=Path,
        help="Optional initial JSONL file. It may be created after startup.",
    )
    parser.add_argument(
        "--watch-dir",
        type=Path,
        help="Root exposed by the UI file selector. Defaults to the initial file's parent or the current directory.",
    )
    parser.add_argument("--host", default="127.0.0.1", help="HTTP bind host. Default: 127.0.0.1")
    parser.add_argument("--port", type=int, default=8765, help="HTTP port. Default: 8765")
    parser.add_argument("--out-dir", type=Path, default=Path("log_viz_live"), help="Root directory for per-log static dashboards")
    parser.add_argument("--output-name", default="dashboard.html", help="Static HTML filename inside each log-specific output directory")
    parser.add_argument("--poll-interval-ms", type=float, default=50.0, help="File-tail polling interval. Default: 50 ms")
    parser.add_argument("--export-idle-seconds", type=float, default=1.0, help="Auto-save when no spans are open and the file is idle this long; 0 disables")
    parser.add_argument("--inline-plotly", action="store_true", help="Embed Plotly.js in both live and saved HTML for offline use")
    parser.add_argument("--open-browser", action="store_true", help="Open the live dashboard in the default browser")
    args = parser.parse_args()

    initial_source = args.log_file.expanduser().resolve(strict=False) if args.log_file else None
    if args.watch_dir:
        watch_root = args.watch_dir.expanduser().resolve(strict=False)
    elif initial_source:
        watch_root = initial_source.parent
    else:
        watch_root = Path.cwd().resolve()
    output_dir = args.out_dir.expanduser().resolve(strict=False)
    watch_root.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    broadcaster = SSEBroadcaster()
    stop_event = threading.Event()
    context = LiveServerContext(
        broadcaster=broadcaster,
        watch_root=watch_root,
        output_dir=output_dir,
        output_name=args.output_name,
        inline_plotly=args.inline_plotly,
        poll_interval=max(0.005, args.poll_interval_ms / 1000.0),
    )
    if initial_source:
        context.attach(initial_source)

    idle_exporter = threading.Thread(
        target=run_idle_exporter,
        args=(context, stop_event, max(0.0, args.export_idle_seconds)),
        name="static-exporter",
        daemon=True,
    )
    idle_exporter.start()

    server = ThreadingHTTPServer((args.host, args.port), make_handler(context))
    server.daemon_threads = True
    url_host = "127.0.0.1" if args.host in {"0.0.0.0", "::"} else args.host
    url = f"http://{url_host}:{args.port}/"

    def request_shutdown(*_: Any) -> None:
        stop_event.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, request_shutdown)
    signal.signal(signal.SIGTERM, request_shutdown)

    print(f"Watch directory: {watch_root}")
    print(f"Live dashboard: {url}")
    print(f"Static report root: {output_dir}")
    if initial_source:
        print(f"Initially attached: {initial_source}")
    else:
        print("No log attached yet; choose one in the dashboard UI.")
    if args.open_browser:
        webbrowser.open(url)

    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        stop_event.set()
        server.server_close()
        context.shutdown()


if __name__ == "__main__":
    main()
