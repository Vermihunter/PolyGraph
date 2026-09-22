#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import heapq
import html
import json
import math
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator, Sequence


AVERAGE_DIR_NAME = "Average"
OUTPUT_FORMAT_VERSION = 2

STRUCTURAL_EVENT_FIELDS = {
    "event",
    # Current logger schema.
    "sequence",
    "elapsed_ns",
    "timestamp_unix_ns",
    # Legacy and normalized aliases.
    "seq",
    "steady_ns",
    "steady_time_ns",
    "monotonic_ns",
    "timestamp_ns",
    "thread_id",
    "time",
    "timestamp",
    "duration_ns",
    "duration_us",
    "duration_ms",
    "depth",
}

ATTRIBUTE_CONTAINER_FIELDS = {"attributes", "fields"}

# Events are assigned a global sequence before concurrent threads append them to
# the JSONL file. Writes can therefore arrive a little out of order. Keep a small
# heap rather than materializing and sorting the complete trace.
DEFAULT_EVENT_REORDER_WINDOW = 4096


def _coerce_int(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return None


def _first_integer(event: dict[str, Any], keys: Sequence[str]) -> tuple[int | None, str | None]:
    for key in keys:
        if key not in event:
            continue
        value = _coerce_int(event.get(key))
        if value is not None:
            return value, key
    return None, None


def normalize_event(
    event: dict[str, Any],
    *,
    path: Path | None = None,
    line_number: int | None = None,
) -> dict[str, Any]:
    """Normalize current and legacy logger fields to ``seq``/``steady_ns``.

    Current PolyGraph traces use ``sequence`` plus ``elapsed_ns`` and may also carry
    ``timestamp_unix_ns``. Older traces used ``seq`` and ``steady_ns``. Keeping
    both aliases lets the dashboard and average-report generator share one input
    contract without rewriting the source JSONL files.
    """
    location = ""
    if path is not None:
        location = str(path)
        if line_number is not None:
            location += f":{line_number}"
        location += ": "

    sequence, _ = _first_integer(event, ("sequence", "seq"))
    if sequence is None:
        raise ValueError(
            f"{location}event must contain an integer 'sequence' (or legacy 'seq')"
        )

    elapsed_ns, timing_field = _first_integer(
        event,
        (
            "elapsed_ns",
            "steady_ns",
            "steady_time_ns",
            "monotonic_ns",
            "timestamp_unix_ns",
            "timestamp_ns",
        ),
    )
    if elapsed_ns is None:
        raise ValueError(
            f"{location}event must contain an integer timing field; expected one of "
            "elapsed_ns, steady_ns, steady_time_ns, monotonic_ns, "
            "timestamp_unix_ns, timestamp_ns"
        )

    normalized = dict(event)
    normalized["sequence"] = sequence
    normalized["seq"] = sequence
    normalized["steady_ns"] = elapsed_ns
    normalized["_timing_field"] = timing_field
    return normalized


def iter_ordered_jsonl_events(
    path: Path,
    *,
    reorder_window: int = DEFAULT_EVENT_REORDER_WINDOW,
) -> Iterator[tuple[int, dict[str, Any]]]:
    """Yield normalized JSONL events in global sequence order.

    ``reorder_window`` is the maximum number of recently read events retained to
    absorb concurrent write reordering. A late event beyond that window is
    reported explicitly instead of silently reconstructing incorrect spans.
    """
    if reorder_window < 1:
        raise ValueError("reorder_window must be at least 1")

    pending: list[tuple[int, int, int, dict[str, Any]]] = []
    arrival_index = 0
    last_emitted_seq: int | None = None

    def emit_next() -> tuple[int, dict[str, Any]]:
        nonlocal last_emitted_seq
        seq, _, line_number, event = heapq.heappop(pending)
        if last_emitted_seq is not None and seq < last_emitted_seq:
            raise ValueError(
                f"{path}:{line_number}: event sequence {seq} arrived more than "
                f"{reorder_window} events late (already emitted {last_emitted_seq}). "
                "Increase DEFAULT_EVENT_REORDER_WINDOW for unusually delayed logs."
            )
        last_emitted_seq = seq
        return line_number, event

    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                raw_event = json.loads(stripped)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {exc}") from exc
            if not isinstance(raw_event, dict):
                raise ValueError(f"{path}:{line_number}: event must be a JSON object")

            event = normalize_event(raw_event, path=path, line_number=line_number)
            seq = int(event["seq"])
            heapq.heappush(pending, (seq, arrival_index, line_number, event))
            arrival_index += 1
            if len(pending) > reorder_window:
                yield emit_next()

    while pending:
        yield emit_next()


@dataclass(frozen=True)
class Span:
    name: str
    start_ms: float
    duration_ms: float
    thread_id: str
    depth: int | None
    fields: dict[str, Any]

    @property
    def end_ms(self) -> float:
        return self.start_ms + self.duration_ms


@dataclass(frozen=True)
class RunProfile:
    path: Path
    spans: tuple[Span, ...]
    window_ms: float


def _base_event_name(event_name: str) -> str:
    if event_name.endswith(".start"):
        return event_name[:-6]
    if event_name.endswith(".end"):
        return event_name[:-4]
    return event_name


def _numeric_depth(value: Any) -> int | None:
    if value is None or value == "":
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _flatten_attribute_value(output: dict[str, Any], key: str, value: Any) -> None:
    if isinstance(value, dict):
        if not value:
            output[key] = "{}"
            return
        for child_key, child_value in value.items():
            child_name = f"{key}.{child_key}" if key else str(child_key)
            _flatten_attribute_value(output, child_name, child_value)
        return
    if isinstance(value, (list, tuple)):
        output[key] = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
        return
    output[key] = value


def _extract_span_attributes(fields: dict[str, Any]) -> dict[str, Any]:
    attributes: dict[str, Any] = {}
    for key, value in fields.items():
        if key in STRUCTURAL_EVENT_FIELDS:
            continue
        if key in ATTRIBUTE_CONTAINER_FIELDS and isinstance(value, dict):
            for child_key, child_value in value.items():
                _flatten_attribute_value(attributes, str(child_key), child_value)
            continue
        _flatten_attribute_value(attributes, str(key), value)
    return attributes


def _aggregate_span_attributes(spans: Sequence[Span]) -> dict[str, Any]:
    """Preserve stable metadata and average numeric metadata in synthetic runs."""
    samples_by_key: dict[str, list[Any]] = defaultdict(list)
    for span in spans:
        for key, value in _extract_span_attributes(span.fields).items():
            if value is not None:
                samples_by_key[key].append(value)

    aggregated: dict[str, Any] = {}
    for key, values in sorted(samples_by_key.items()):
        if not values:
            continue
        if all(value == values[0] for value in values):
            aggregated[key] = values[0]
            continue
        if all(isinstance(value, (int, float)) and not isinstance(value, bool) for value in values):
            aggregated[key] = _mean([float(value) for value in values])
            continue

        distinct = []
        for value in values:
            rendered = str(value)
            if rendered not in distinct:
                distinct.append(rendered)
        if len(distinct) <= 4:
            aggregated[key] = "mixed: " + " | ".join(distinct)
        else:
            aggregated[key] = f"mixed ({len(distinct)} values)"
    return aggregated


def load_run(path: Path) -> RunProfile:
    """Load a log and pair spans while tolerating concurrent write reordering."""
    stacks: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    raw_spans: list[tuple[str, int, int, str, dict[str, Any]]] = []
    event_count = 0

    for _, event in iter_ordered_jsonl_events(path):
        event_count += 1
        event_name = str(event.get("event", ""))
        if not event_name:
            continue
        thread_id = str(event.get("thread_id", "0"))
        base_name = _base_event_name(event_name)
        key = (thread_id, base_name)

        if event_name.endswith(".start"):
            stacks[key].append(event)
            continue
        if not event_name.endswith(".end") or not stacks[key]:
            continue

        start_event = stacks[key].pop()
        try:
            start_ns = int(start_event["steady_ns"])
            end_ns = int(event["steady_ns"])
        except (KeyError, TypeError, ValueError):
            continue
        if end_ns < start_ns:
            continue

        merged = dict(start_event)
        merged.update(event)
        compact_fields = _extract_span_attributes(merged)
        depth = _numeric_depth(merged.get("depth"))
        if depth is not None:
            compact_fields["depth"] = depth
        raw_spans.append((base_name, start_ns, end_ns, thread_id, compact_fields))

    if event_count == 0:
        raise ValueError(f"No events found in {path}")
    if not raw_spans:
        raise ValueError(f"No .start/.end spans could be reconstructed from {path}")

    raw_spans.sort(key=lambda item: (item[1], item[2], item[0]))
    origin_ns = min(start_ns for _, start_ns, _, _, _ in raw_spans)
    spans = tuple(
        Span(
            name=name,
            start_ms=(start_ns - origin_ns) / 1_000_000.0,
            duration_ms=(end_ns - start_ns) / 1_000_000.0,
            thread_id=thread_id,
            depth=_numeric_depth(fields.get("depth")),
            fields=fields,
        )
        for name, start_ns, end_ns, thread_id, fields in raw_spans
    )
    window_ms = max(span.end_ms for span in spans) - min(span.start_ms for span in spans)
    return RunProfile(path=path, spans=spans, window_ms=window_ms)


def percentile(values: Sequence[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _mean(values: Sequence[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def _stdev(values: Sequence[float]) -> float:
    return statistics.stdev(values) if len(values) > 1 else 0.0


def summarize_events(runs: Sequence[RunProfile]) -> list[dict[str, Any]]:
    durations_by_run: list[dict[str, list[float]]] = []
    names: set[str] = set()
    for run in runs:
        grouped: dict[str, list[float]] = defaultdict(list)
        for span in run.spans:
            grouped[span.name].append(span.duration_ms)
        durations_by_run.append(grouped)
        names.update(grouped)

    rows: list[dict[str, Any]] = []
    for name in sorted(names):
        totals_per_run: list[float] = []
        counts_per_run: list[int] = []
        span_durations: list[float] = []
        present_runs = 0

        for grouped in durations_by_run:
            selected = grouped.get(name, ())
            totals_per_run.append(sum(selected))
            counts_per_run.append(len(selected))
            span_durations.extend(selected)
            if selected:
                present_runs += 1

        rows.append(
            {
                "event": name,
                "runs": len(runs),
                "present_runs": present_runs,
                "presence_pct": 100.0 * present_runs / len(runs),
                "avg_count_per_run": _mean([float(value) for value in counts_per_run]),
                "avg_total_ms_per_run": _mean(totals_per_run),
                "median_total_ms_per_run": statistics.median(totals_per_run),
                "p95_total_ms_per_run": percentile(totals_per_run, 0.95),
                "stddev_total_ms_per_run": _stdev(totals_per_run),
                "min_total_ms_per_run": min(totals_per_run),
                "max_total_ms_per_run": max(totals_per_run),
                "avg_span_ms": _mean(span_durations),
                "p95_span_ms": percentile(span_durations, 0.95),
                "span_count": len(span_durations),
            }
        )

    inclusive_total = sum(float(row["avg_total_ms_per_run"]) for row in rows)
    for row in rows:
        row["inclusive_share_pct"] = (
            100.0 * float(row["avg_total_ms_per_run"]) / inclusive_total
            if inclusive_total > 0.0
            else 0.0
        )
    return sorted(rows, key=lambda row: float(row["avg_total_ms_per_run"]), reverse=True)


def build_average_timeline(runs: Sequence[RunProfile]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, int | None, int], list[Span]] = defaultdict(list)

    for run in runs:
        occurrence_by_key: dict[tuple[str, int | None], int] = defaultdict(int)
        for span in sorted(run.spans, key=lambda value: (value.start_ms, value.end_ms, value.name)):
            base_key = (span.name, span.depth)
            occurrence = occurrence_by_key[base_key]
            occurrence_by_key[base_key] += 1
            grouped[(span.name, span.depth, occurrence)].append(span)

    rows: list[dict[str, Any]] = []
    for (name, depth, occurrence), samples in grouped.items():
        starts = [sample.start_ms for sample in samples]
        durations = [sample.duration_ms for sample in samples]
        row = {
                "event": name,
                "depth": "" if depth is None else depth,
                "occurrence": occurrence + 1,
                "sample_runs": len(samples),
                "presence_pct": 100.0 * len(samples) / len(runs),
                "avg_start_ms": _mean(starts),
                "avg_duration_ms": _mean(durations),
                "p95_duration_ms": percentile(durations, 0.95),
                "stddev_duration_ms": _stdev(durations),
        }
        row.update(_aggregate_span_attributes(samples))
        rows.append(row)

    return sorted(
        rows,
        key=lambda row: (
            float(row["avg_start_ms"]),
            str(row["event"]),
            int(row["occurrence"]),
        ),
    )


def write_csv(path: Path, rows: Sequence[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames: list[str] = []
    seen: set[str] = set()
    for row in rows:
        for key in row:
            if key not in seen:
                seen.add(key)
                fieldnames.append(key)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_average_jsonl(path: Path, timeline: Sequence[dict[str, Any]], run_count: int) -> None:
    synthetic: list[dict[str, Any]] = []
    for lane, row in enumerate(timeline, start=1):
        start_ns = max(0, round(float(row["avg_start_ms"]) * 1_000_000.0))
        duration_ns = max(0, round(float(row["avg_duration_ms"]) * 1_000_000.0))
        common: dict[str, Any] = {
            "thread_id": lane,
            "average_runs": run_count,
            "sample_runs": int(row["sample_runs"]),
            "presence_pct": float(row["presence_pct"]),
            "occurrence": int(row["occurrence"]),
        }
        if row["depth"] != "":
            common["depth"] = int(row["depth"])

        timeline_fields = {
            "event",
            "depth",
            "occurrence",
            "sample_runs",
            "presence_pct",
            "avg_start_ms",
            "avg_duration_ms",
            "p95_duration_ms",
            "stddev_duration_ms",
        }
        for key, value in row.items():
            if key not in timeline_fields:
                common[key] = value

        synthetic.append(
            {
                **common,
                "event": f"{row['event']}.start",
                "steady_ns": start_ns,
                "time": start_ns,
                "_sort_end": 0,
            }
        )
        synthetic.append(
            {
                **common,
                "event": f"{row['event']}.end",
                "steady_ns": start_ns + duration_ns,
                "time": start_ns + duration_ns,
                "duration_ms": float(row["avg_duration_ms"]),
                "p95_duration_ms": float(row["p95_duration_ms"]),
                "_sort_end": 1,
            }
        )

    synthetic.sort(key=lambda event: (int(event["steady_ns"]), int(event["_sort_end"])))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for sequence, event in enumerate(synthetic):
            event = dict(event)
            event.pop("_sort_end", None)
            event["sequence"] = sequence
            handle.write(json.dumps(event, separators=(",", ":")) + "\n")


def _event_colour(name: str) -> str:
    hue = sum((index + 1) * ord(character) for index, character in enumerate(name)) % 360
    return f"hsl({hue} 72% 58%)"


def _format_ms(value: float) -> str:
    if value >= 1000.0:
        return f"{value / 1000.0:.3f} s"
    if value >= 1.0:
        return f"{value:.3f} ms"
    return f"{value * 1000.0:.3f} µs"


def build_report_html(
    *,
    title: str,
    runs: Sequence[RunProfile],
    event_rows: Sequence[dict[str, Any]],
    timeline_rows: Sequence[dict[str, Any]],
    source_label: str,
) -> str:
    windows = [run.window_ms for run in runs]
    avg_window = _mean(windows)
    p95_window = percentile(windows, 0.95)
    std_window = _stdev(windows)
    total_spans = sum(len(run.spans) for run in runs)

    top_rows = list(event_rows[:20])
    max_bar = max((float(row["avg_total_ms_per_run"]) for row in top_rows), default=1.0)
    bars = "\n".join(
        f"""
        <div class="bar-row" title="{html.escape(str(row['event']))}: {_format_ms(float(row['avg_total_ms_per_run']))}">
          <div class="bar-label">{html.escape(str(row['event']))}</div>
          <div class="bar-track"><div class="bar" style="width:{100.0 * float(row['avg_total_ms_per_run']) / max_bar:.4f}%;background:{_event_colour(str(row['event']))}"></div></div>
          <div class="bar-value">{_format_ms(float(row['avg_total_ms_per_run']))}</div>
        </div>
        """
        for row in top_rows
    )

    event_table_rows = "\n".join(
        f"""
        <tr>
          <td>{html.escape(str(row['event']))}</td>
          <td data-sort="{float(row['avg_total_ms_per_run']):.12f}">{float(row['avg_total_ms_per_run']):.4f}</td>
          <td data-sort="{float(row['p95_total_ms_per_run']):.12f}">{float(row['p95_total_ms_per_run']):.4f}</td>
          <td data-sort="{float(row['stddev_total_ms_per_run']):.12f}">{float(row['stddev_total_ms_per_run']):.4f}</td>
          <td>{float(row['avg_count_per_run']):.2f}</td>
          <td>{float(row['avg_span_ms']):.4f}</td>
          <td>{float(row['presence_pct']):.1f}%</td>
          <td>{float(row['inclusive_share_pct']):.2f}%</td>
        </tr>
        """
        for row in event_rows
    )

    timeline_limit = 250
    visible_timeline = list(timeline_rows[:timeline_limit])
    timeline_end = max(
        (
            float(row["avg_start_ms"]) + float(row["avg_duration_ms"])
            for row in visible_timeline
        ),
        default=1.0,
    )
    timeline_html = "\n".join(
        f"""
        <div class="timeline-row">
          <div class="timeline-label" title="{html.escape(str(row['event']))}">
            {html.escape(str(row['event']))}
            <span>{'depth ' + str(row['depth']) if row['depth'] != '' else ''} · #{row['occurrence']}</span>
          </div>
          <div class="timeline-track">
            <div class="timeline-span" style="left:{100.0 * float(row['avg_start_ms']) / timeline_end:.5f}%;width:{max(0.15, 100.0 * float(row['avg_duration_ms']) / timeline_end):.5f}%;background:{_event_colour(str(row['event']))}" title="start {float(row['avg_start_ms']):.3f} ms · average {float(row['avg_duration_ms']):.3f} ms · p95 {float(row['p95_duration_ms']):.3f} ms · present in {float(row['presence_pct']):.1f}% of runs"></div>
          </div>
        </div>
        """
        for row in visible_timeline
    )

    truncated_note = (
        f"Showing the first {timeline_limit} of {len(timeline_rows)} averaged span occurrences."
        if len(timeline_rows) > timeline_limit
        else f"Showing all {len(timeline_rows)} averaged span occurrences."
    )

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<style>
:root{{--bg:#07111f;--panel:#0e1b2e;--panel2:#14243a;--text:#eef5ff;--muted:#94a8c6;--line:#263a56;--accent:#75a7ff;--good:#53d39b}}
*{{box-sizing:border-box}}body{{margin:0;background:radial-gradient(circle at 15% 0%,#18335b 0,#07111f 36rem);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}}
main{{max-width:1500px;margin:auto;padding:34px}}h1{{font-size:clamp(2rem,4vw,3.6rem);margin:0 0 8px;letter-spacing:-.045em}}h2{{margin:0 0 18px;font-size:1.25rem}}p{{color:var(--muted)}}.eyebrow{{color:#8cb5ff;font-weight:750;text-transform:uppercase;letter-spacing:.14em;font-size:.75rem}}
.cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:14px;margin:26px 0}}.card,.section{{background:linear-gradient(145deg,rgba(20,36,58,.95),rgba(10,24,42,.94));border:1px solid rgba(130,165,215,.18);box-shadow:0 18px 60px rgba(0,0,0,.22);border-radius:18px}}.card{{padding:18px}}.card .value{{font-size:1.65rem;font-weight:800;margin-top:6px}}.card .label{{font-size:.78rem;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}}.section{{padding:22px;margin:18px 0}}
.bar-row{{display:grid;grid-template-columns:minmax(170px,320px) 1fr 105px;gap:12px;align-items:center;margin:10px 0}}.bar-label{{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;color:#dbe9ff;font-size:.9rem}}.bar-track{{height:15px;background:#081321;border:1px solid #263a56;border-radius:99px;overflow:hidden}}.bar{{height:100%;border-radius:99px;box-shadow:0 0 18px currentColor}}.bar-value{{font-variant-numeric:tabular-nums;color:#bed1ef;text-align:right;font-size:.85rem}}
.table-tools{{display:flex;justify-content:space-between;gap:12px;align-items:center;margin-bottom:12px}}input{{width:min(420px,100%);padding:10px 12px;border-radius:10px;border:1px solid var(--line);background:#071321;color:var(--text)}}.table-wrap{{overflow:auto;max-height:720px;border:1px solid var(--line);border-radius:12px}}table{{width:100%;border-collapse:collapse;font-size:.85rem}}thead{{position:sticky;top:0;background:#172943;z-index:1}}th,td{{padding:10px 12px;border-bottom:1px solid rgba(66,91,128,.42);white-space:nowrap;text-align:right}}th:first-child,td:first-child{{text-align:left}}th{{cursor:pointer;color:#cfe0fa}}tbody tr:hover{{background:rgba(77,126,196,.12)}}
.timeline{{min-width:900px}}.timeline-row{{display:grid;grid-template-columns:300px 1fr;gap:12px;min-height:27px;align-items:center}}.timeline-label{{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:.78rem;color:#d8e8ff}}.timeline-label span{{color:#6f87a8;margin-left:8px}}.timeline-track{{height:16px;background:repeating-linear-gradient(90deg,#0a1728 0,#0a1728 calc(10% - 1px),#263a56 10%);position:relative;border-radius:4px;overflow:hidden}}.timeline-span{{position:absolute;height:100%;min-width:2px;border-radius:4px;opacity:.9}}.note{{font-size:.82rem;color:var(--muted)}}code{{color:#a9c9ff}}@media(max-width:800px){{main{{padding:18px}}.bar-row{{grid-template-columns:130px 1fr 82px}}.timeline-row{{grid-template-columns:210px 1fr}}}}
</style>
</head>
<body><main>
<div class="eyebrow">PolyGraph averaged event profile</div>
<h1>{html.escape(title)}</h1>
<p>Source: <code>{html.escape(source_label)}</code>. Timings are reconstructed from matching <code>.start</code>/<code>.end</code> events. Event totals are inclusive and may overlap.</p>
<div class="cards">
  <div class="card"><div class="label">Executions</div><div class="value">{len(runs)}</div></div>
  <div class="card"><div class="label">Average execution window</div><div class="value">{_format_ms(avg_window)}</div></div>
  <div class="card"><div class="label">P95 execution window</div><div class="value">{_format_ms(p95_window)}</div></div>
  <div class="card"><div class="label">Window standard deviation</div><div class="value">{_format_ms(std_window)}</div></div>
  <div class="card"><div class="label">Reconstructed spans</div><div class="value">{total_spans:,}</div></div>
  <div class="card"><div class="label">Event types</div><div class="value">{len(event_rows)}</div></div>
</div>
<section class="section"><h2>Average inclusive time per execution</h2>{bars}</section>
<section class="section"><div class="table-tools"><div><h2>Event timing table</h2><div class="note">Click a column header to sort.</div></div><input id="filter" placeholder="Filter event names…"></div><div class="table-wrap"><table id="eventTable"><thead><tr><th>Event</th><th>Avg total/run ms</th><th>P95 total/run ms</th><th>Stddev ms</th><th>Avg count/run</th><th>Avg span ms</th><th>Presence</th><th>Inclusive share</th></tr></thead><tbody>{event_table_rows}</tbody></table></div></section>
<section class="section"><h2>Synthetic average execution timeline</h2><p class="note">{html.escape(truncated_note)} Each occurrence is aligned by event name, BFS depth, and occurrence order, then averaged across executions.</p><div class="table-wrap"><div class="timeline">{timeline_html}</div></div></section>
</main>
<script>
const table=document.getElementById('eventTable');const filter=document.getElementById('filter');filter.addEventListener('input',()=>{{const q=filter.value.toLowerCase();table.querySelectorAll('tbody tr').forEach(r=>r.hidden=!r.cells[0].textContent.toLowerCase().includes(q));}});table.querySelectorAll('th').forEach((th,i)=>{{let asc=false;th.addEventListener('click',()=>{{asc=!asc;const rows=[...table.tBodies[0].rows];rows.sort((a,b)=>{{const av=a.cells[i].dataset.sort??a.cells[i].textContent;const bv=b.cells[i].dataset.sort??b.cells[i].textContent;const an=Number(av),bn=Number(bv);const cmp=Number.isNaN(an)||Number.isNaN(bn)?av.localeCompare(bv):an-bn;return asc?cmp:-cmp;}});rows.forEach(r=>table.tBodies[0].appendChild(r));}});}});
</script></body></html>"""


def _cached_group_summary(
        log_files: Sequence[Path],
        output_dir: Path,
) -> dict[str, Any] | None:
    required = [
        output_dir / "summary.json",
        output_dir / "events.csv",
        output_dir / "timeline.csv",
        output_dir / "execution.jsonl",
        output_dir / "report.html",
    ]
    if not log_files or not all(path.is_file() for path in required):
        return None
    newest_source = max(path.stat().st_mtime_ns for path in log_files)
    oldest_output = min(path.stat().st_mtime_ns for path in required)
    if oldest_output < newest_source:
        return None
    try:
        summary = json.loads((output_dir / "summary.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if summary.get("formatVersion") != OUTPUT_FORMAT_VERSION:
        return None
    summary["cached"] = True
    return summary


def analyze_group(log_files: Sequence[Path], output_dir: Path, *, title: str, source_label: str) -> dict[str, Any]:
    cached = _cached_group_summary(log_files, output_dir)
    if cached is not None:
        return cached

    runs: list[RunProfile] = []
    errors: list[dict[str, str]] = []
    for log_file in log_files:
        try:
            runs.append(load_run(log_file))
        except Exception as exc:  # noqa: BLE001 - report malformed logs without hiding valid ones
            errors.append({"log": str(log_file), "error": str(exc)})

    if not runs:
        return {
            "formatVersion": OUTPUT_FORMAT_VERSION,
            "generated": False,
            "title": title,
            "validRuns": len(runs),
            "invalidRuns": len(errors),
            "errors": errors,
        }

    output_dir.mkdir(parents=True, exist_ok=True)
    event_rows = summarize_events(runs)
    timeline_rows = build_average_timeline(runs)
    windows = [run.window_ms for run in runs]

    write_csv(output_dir / "events.csv", event_rows)
    write_csv(output_dir / "timeline.csv", timeline_rows)
    write_average_jsonl(output_dir / "execution.jsonl", timeline_rows, len(runs))
    (output_dir / "report.html").write_text(
        build_report_html(
            title=title,
            runs=runs,
            event_rows=event_rows,
            timeline_rows=timeline_rows,
            source_label=source_label,
        ),
        encoding="utf-8",
    )

    summary = {
        "formatVersion": OUTPUT_FORMAT_VERSION,
        "generated": True,
        "mode": "average" if len(runs) > 1 else "single-run",
        "title": title,
        "source": source_label,
        "validRuns": len(runs),
        "invalidRuns": len(errors),
        "averageWindowMs": _mean(windows),
        "medianWindowMs": statistics.median(windows),
        "p95WindowMs": percentile(windows, 0.95),
        "stddevWindowMs": _stdev(windows),
        "minWindowMs": min(windows),
        "maxWindowMs": max(windows),
        "eventTypeCount": len(event_rows),
        "spanCount": sum(len(run.spans) for run in runs),
        "outputs": {
            "report": "report.html",
            "events": "events.csv",
            "timeline": "timeline.csv",
            "averageExecution": "execution.jsonl",
        },
        "errors": errors,
    }
    (output_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def discover_direct_log_groups(log_root: Path) -> list[tuple[Path, list[Path]]]:
    groups: list[tuple[Path, list[Path]]] = []
    for directory in sorted(path for path in log_root.rglob("*") if path.is_dir()):
        if AVERAGE_DIR_NAME in directory.parts:
            continue
        logs = sorted(
            path
            for path in directory.glob("*.jsonl")
            if path.is_file() and path.name != "execution.jsonl"
        )
        if logs:
            groups.append((directory, logs))
    return groups


def _all_source_logs(log_root: Path) -> list[Path]:
    return sorted(
        path
        for path in log_root.rglob("*.jsonl")
        if path.is_file()
        and AVERAGE_DIR_NAME not in path.parts
        and path.name != "execution.jsonl"
    )


def build_index_html(output_root: Path, summaries: Sequence[tuple[str, Path, dict[str, Any]]]) -> str:
    valid = [(label, path, summary) for label, path, summary in summaries if summary.get("generated")]
    max_window = max((float(summary["averageWindowMs"]) for _, _, summary in valid), default=1.0)
    cards = "\n".join(
        f"""
        <a class="group" href="{html.escape(str((path / 'report.html').relative_to(output_root)))}">
          <div><strong>{html.escape(label)}</strong><span>{int(summary['validRuns'])} runs · {int(summary['eventTypeCount'])} event types</span></div>
          <div class="mini"><i style="width:{100.0 * float(summary['averageWindowMs']) / max_window:.4f}%"></i></div>
          <b>{_format_ms(float(summary['averageWindowMs']))}</b>
        </a>
        """
        for label, path, summary in valid
    )
    content = cards or """
        <div class="empty">
          <strong>No valid execution profiles were generated.</strong>
          <span>Check the reconstruction output for malformed or unsupported JSONL logs.</span>
        </div>
    """
    return f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>PolyGraph average executions</title><style>body{{margin:0;background:#07111f;color:#eaf3ff;font-family:system-ui,sans-serif}}main{{max-width:1100px;margin:auto;padding:42px}}h1{{font-size:3rem;margin:.2em 0}}p,span{{color:#90a7c6}}.group{{display:grid;grid-template-columns:minmax(220px,1fr) 2fr 110px;gap:18px;align-items:center;text-decoration:none;color:inherit;padding:18px 20px;margin:12px 0;border-radius:15px;background:#10213a;border:1px solid #29405f}}.group:hover{{transform:translateY(-1px);border-color:#6c9de2}}.group strong,.group span,.empty strong,.empty span{{display:block}}.group span,.empty span{{font-size:.82rem;margin-top:4px}}.mini{{height:12px;background:#071321;border-radius:99px;overflow:hidden}}.mini i{{display:block;height:100%;background:linear-gradient(90deg,#53d39b,#75a7ff);border-radius:99px}}b{{text-align:right}}.empty{{padding:22px;border-radius:15px;background:#10213a;border:1px solid #29405f}}@media(max-width:700px){{.group{{grid-template-columns:1fr}}b{{text-align:left}}}}</style></head><body><main><div style="color:#83adf0;text-transform:uppercase;letter-spacing:.15em;font-size:.75rem;font-weight:700">PolyGraph measurements</div><h1>Average executions</h1><p>Open a group to inspect its event table, average timing bars, and synthetic average timeline.</p>{content}</main></body></html>"""


def _safe_relative_output(relative: Path) -> Path:
    if str(relative) in {"", "."}:
        return Path("root")
    return relative


def analyze_log_tree(log_root: Path, *, output_root: Path | None = None) -> dict[str, Any]:
    """Analyze source logs without writing anything into the source tree."""
    log_root = log_root.expanduser().resolve()
    if not log_root.is_dir():
        raise FileNotFoundError(f"Log directory does not exist: {log_root}")

    if output_root is None:
        output_root = log_root.parent / "log_reconstruction"
    output_root = output_root.expanduser().resolve()
    if output_root == log_root or output_root.is_relative_to(log_root):
        raise ValueError(
            "Average output must be outside the source log directory: "
            f"logs={log_root}, output={output_root}"
        )
    output_root.mkdir(parents=True, exist_ok=True)
    averages_root = output_root / "averages"

    summaries: list[tuple[str, Path, dict[str, Any]]] = []
    by_group_rows: list[dict[str, Any]] = []

    for directory, log_files in discover_direct_log_groups(log_root):
        relative = directory.relative_to(log_root)
        label = str(relative) if str(relative) != "." else "logs"
        output_dir = averages_root / _safe_relative_output(relative)
        summary = analyze_group(
            log_files,
            output_dir,
            title=f"{'Average execution' if len(log_files) > 1 else 'Execution'} — {label}",
            source_label=str(relative),
        )
        summaries.append((label, output_dir, summary))
        if summary.get("generated"):
            by_group_rows.append(
                {
                    "group": label,
                    "runs": summary["validRuns"],
                    "average_window_ms": summary["averageWindowMs"],
                    "median_window_ms": summary["medianWindowMs"],
                    "p95_window_ms": summary["p95WindowMs"],
                    "stddev_window_ms": summary["stddevWindowMs"],
                    "min_window_ms": summary["minWindowMs"],
                    "max_window_ms": summary["maxWindowMs"],
                    "event_types": summary["eventTypeCount"],
                    "spans": summary["spanCount"],
                    "report": str((output_dir / "report.html").relative_to(output_root)),
                }
            )

    all_logs = _all_source_logs(log_root)
    global_output = averages_root / "Global"
    global_summary = analyze_group(
        all_logs,
        global_output,
        title="Global average execution — all logs",
        source_label=str(log_root),
    )
    summaries.insert(0, ("Global — all logs", global_output, global_summary))

    group_table = output_root / "average_by_log_directory.csv"
    write_csv(group_table, by_group_rows)
    index_path = output_root / "average_index.html"
    index_path.write_text(build_index_html(output_root, summaries), encoding="utf-8")

    manifest = {
        "logRoot": str(log_root),
        "outputRoot": str(output_root),
        "sourceLogCount": len(all_logs),
        "directoryAverageCount": len(by_group_rows),
        "globalAverageGenerated": bool(global_summary.get("generated")),
        "index": str(index_path),
        "groupTable": str(group_table),
        "groups": [
            {
                "label": label,
                "output": str(output_dir),
                **summary,
            }
            for label, output_dir, summary in summaries
        ],
    }
    (output_root / "average_analysis.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Average PolyGraph event logs per log directory and globally, then generate "
            "CSV tables, synthetic average JSONL executions, and standalone HTML visualizations."
        )
    )
    parser.add_argument("logs", type=Path, help="Root logs directory, e.g. measurements/<run-id>/logs")
    parser.add_argument(
        "--out-dir",
        type=Path,
        help="Output directory. Defaults to <measurement>/log_reconstruction.",
    )
    args = parser.parse_args()
    result = analyze_log_tree(args.logs, output_root=args.out_dir)
    print(f"Average index: {result['index']}")
    print(f"Group table:   {result['groupTable']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
