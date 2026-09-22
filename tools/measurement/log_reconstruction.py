#!/usr/bin/env python3

import argparse
import html
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import pandas as pd
import plotly.graph_objects as go


PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.event_log_average import AVERAGE_DIR_NAME, analyze_log_tree, normalize_event


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
    "_timing_field",
    "thread_id",
    "time",
    "timestamp",
    "duration_ns",
    "duration_us",
    "duration_ms",
}

ATTRIBUTE_CONTAINER_FIELDS = {"attributes", "fields"}

PREFERRED_ATTRIBUTE_ORDER = (
    "depth",
    "backend",
    "source_backend",
    "target_backend",
    "frontier_size",
    "row_size",
    "batch_size",
    "source_rows",
    "result_count",
)


def _flatten_attribute_value(
        output: dict[str, Any],
        key: str,
        value: Any,
) -> None:
    """Flatten nested event metadata while keeping arrays readable."""
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


def is_missing_value(value: Any) -> bool:
    if value is None:
        return True
    if isinstance(value, (dict, list, tuple, set)):
        return False
    try:
        missing = pd.isna(value)
    except (TypeError, ValueError):
        return False
    try:
        return bool(missing)
    except (TypeError, ValueError):
        return False


def span_field_value(
        start_event: dict[str, Any],
        end_event: dict[str, Any],
        key: str,
) -> Any:
    end_value = end_event.get(key)
    if not is_missing_value(end_value):
        return end_value
    start_value = start_event.get(key)
    if not is_missing_value(start_value):
        return start_value
    return None


def extract_event_attributes(event: dict[str, Any]) -> dict[str, Any]:
    """Return every non-structural field attached to a log event.

    Some logger implementations put custom fields directly on the event while
    others wrap them in ``fields`` or ``attributes``. Both representations are
    normalized into the same flat attribute namespace.
    """
    attributes: dict[str, Any] = {}
    for key, value in event.items():
        if key in STRUCTURAL_EVENT_FIELDS:
            continue
        if is_missing_value(value):
            continue
        if key in ATTRIBUTE_CONTAINER_FIELDS and isinstance(value, dict):
            for child_key, child_value in value.items():
                _flatten_attribute_value(attributes, str(child_key), child_value)
            continue
        _flatten_attribute_value(attributes, str(key), value)
    return attributes


def merge_span_attributes(
        start_event: dict[str, Any],
        end_event: dict[str, Any],
) -> dict[str, Any]:
    """Merge metadata from both boundaries without hiding changed values."""
    start_attributes = extract_event_attributes(start_event)
    end_attributes = extract_event_attributes(end_event)
    merged: dict[str, Any] = {}

    for key in sorted(set(start_attributes) | set(end_attributes)):
        in_start = key in start_attributes
        in_end = key in end_attributes
        if in_start and in_end and start_attributes[key] != end_attributes[key]:
            merged[f"start.{key}"] = start_attributes[key]
            merged[f"end.{key}"] = end_attributes[key]
        elif in_end:
            merged[key] = end_attributes[key]
        else:
            merged[key] = start_attributes[key]
    return merged


def _attribute_sort_key(key: str) -> tuple[int, str]:
    try:
        return PREFERRED_ATTRIBUTE_ORDER.index(key), key
    except ValueError:
        return len(PREFERRED_ATTRIBUTE_ORDER), key


def format_attribute_value(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def event_name_prefix(event_name: str) -> str:
    """Return the first namespace component used by timeline family filters."""
    prefix, separator, _ = str(event_name).partition(".")
    return prefix if separator else str(event_name)


def attribute_filter_token(value: Any) -> str:
    """Create a type-aware browser-stable token for an attribute value."""
    if isinstance(value, bool):
        return f"boolean:{str(value).lower()}"
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return f"number:{json.dumps(value, separators=(',', ':'))}"
    if isinstance(value, str):
        return f"string:{value}"
    return "json:" + json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        default=str,
    )


def attribute_filter_label(value: Any) -> str:
    if isinstance(value, bool):
        return "True" if value else "False"
    return format_attribute_value(value)


def build_timeline_filter_metadata(spans: pd.DataFrame) -> dict[str, Any]:
    """Describe event families and categorical attribute values for the UI."""
    prefix_counts: dict[str, int] = defaultdict(int)
    event_duration_stats: dict[str, dict[str, Any]] = {}
    attribute_values: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    attribute_presence: dict[str, int] = defaultdict(int)

    for _, span in spans.iterrows():
        event_name = str(span["name"])
        prefix_counts[event_name_prefix(event_name)] += 1

        duration_ms = float(span["duration_ms"])
        duration_entry = event_duration_stats.setdefault(
            event_name,
            {
                "name": event_name,
                "count": 0,
                "minDurationMs": duration_ms,
                "maxDurationMs": duration_ms,
            },
        )
        duration_entry["count"] += 1
        duration_entry["minDurationMs"] = min(
            duration_entry["minDurationMs"], duration_ms
        )
        duration_entry["maxDurationMs"] = max(
            duration_entry["maxDurationMs"], duration_ms
        )

        attributes = span.get("attributes")
        if not isinstance(attributes, dict):
            continue
        for key, value in attributes.items():
            token = attribute_filter_token(value)
            entry = attribute_values[str(key)].setdefault(
                token,
                {"token": token, "label": attribute_filter_label(value), "count": 0},
            )
            entry["count"] += 1
            attribute_presence[str(key)] += 1

    total_spans = len(spans)
    attributes: dict[str, Any] = {}
    for key in sorted(attribute_values, key=_attribute_sort_key):
        values = sorted(
            attribute_values[key].values(),
            key=lambda item: (str(item["label"]).lower(), str(item["token"])),
        )
        attributes[key] = {
            "values": values,
            "missingCount": total_spans - attribute_presence[key],
        }

    return {
        "prefixes": [
            {"name": prefix, "count": count}
            for prefix, count in sorted(prefix_counts.items(), key=lambda item: item[0].lower())
        ],
        "events": sorted(
            event_duration_stats.values(),
            key=lambda item: str(item["name"]).lower(),
        ),
        "attributes": attributes,
        "spanCount": total_spans,
    }


def safe_json_for_html(value: Any) -> str:
    return (
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        .replace("&", "\\u0026")
        .replace("<", "\\u003c")
        .replace(">", "\\u003e")
    )


# ============================================================
# Discovery
# ============================================================

def discover_log_files(path: Path, pattern: str = "*.jsonl") -> list[Path]:
    if path.is_file():
        return [path]

    if not path.exists():
        raise FileNotFoundError(path)

    files = sorted(path.rglob(pattern))

    # Never treat generated average executions or visualizations as source logs.
    files = [
        f for f in files
        if "log_viz" not in f.parts
           and AVERAGE_DIR_NAME not in f.parts
           and f.name != "execution.jsonl"
           and f.is_file()
    ]

    return files


def make_run_id(log_file: Path, root: Path) -> str:
    try:
        rel = log_file.relative_to(root)
    except ValueError:
        rel = log_file.name

    s = str(rel)
    s = s.replace("/", "__")
    s = s.replace("\\", "__")
    s = s.removesuffix(".jsonl")
    s = s.removesuffix(".events")

    return s


def make_safe_html_id(s: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9_-]+", "_", s)
    if not s:
        s = "run"
    if s[0].isdigit():
        s = "run_" + s
    return s


def resolve_log_root(path: Path) -> Path:
    path = path.expanduser().resolve()
    if path.is_dir() and (path / "logs").is_dir():
        return path / "logs"
    return path


def average_execution_entries(average_analysis: dict | None) -> list[tuple[Path, str]]:
    if not average_analysis:
        return []

    entries: list[tuple[Path, str]] = []
    for group in average_analysis.get("groups", []):
        if not group.get("generated"):
            continue
        execution_file = Path(group["output"]) / "execution.jsonl"
        if not execution_file.is_file():
            continue
        label = str(group.get("label", execution_file.parent.name))
        entries.append((execution_file, f"Average — {label}"))
    return entries


# ============================================================
# Loading
# ============================================================

def load_jsonl(path: Path) -> pd.DataFrame:
    """Load current or legacy event JSONL into the normalized dashboard schema."""
    rows = []

    with path.open("r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, start=1):
            line = line.strip()

            if not line:
                continue

            try:
                raw_event = json.loads(line)
            except json.JSONDecodeError as e:
                raise ValueError(f"Invalid JSON on line {line_no}: {e}") from e
            if not isinstance(raw_event, dict):
                raise ValueError(f"Invalid event on line {line_no}: expected an object")
            rows.append(normalize_event(raw_event, path=path, line_number=line_no))

    if not rows:
        raise ValueError(f"No events found in {path}")

    df = pd.DataFrame(rows)

    required = {"event", "seq", "steady_ns", "thread_id"}
    missing = required - set(df.columns)

    if missing:
        raise ValueError(f"Missing required columns after normalization: {missing}")

    df = df.sort_values("seq").reset_index(drop=True)
    return df


# ============================================================
# Span reconstruction
# ============================================================

def base_event_name(event_name: str) -> str:
    if event_name.endswith(".start"):
        return event_name[:-len(".start")]

    if event_name.endswith(".end"):
        return event_name[:-len(".end")]

    return event_name


def pair_spans(events: pd.DataFrame) -> pd.DataFrame:
    stacks = defaultdict(list)
    spans = []
    unmatched_ends = []

    for _, row in events.iterrows():
        event_name = row["event"]
        thread_id = row["thread_id"]
        base_name = base_event_name(event_name)

        if event_name.endswith(".start"):
            key = (thread_id, base_name)
            stacks[key].append(row.to_dict())

        elif event_name.endswith(".end"):
            key = (thread_id, base_name)

            if not stacks[key]:
                unmatched_ends.append(row.to_dict())
                continue

            start_row = stacks[key].pop()
            end_row = row.to_dict()

            start_ns = int(start_row["steady_ns"])
            end_ns = int(end_row["steady_ns"])

            if end_ns < start_ns:
                continue

            merged = {}
            merged.update(start_row)
            merged.update(end_row)
            attributes = merge_span_attributes(start_row, end_row)

            duration_ns = end_ns - start_ns

            span = {
                "name": base_name,
                "thread_id": thread_id,

                "seq_start": start_row.get("seq"),
                "seq_end": end_row.get("seq"),

                "start_ns": start_ns,
                "end_ns": end_ns,

                "duration_ns": duration_ns,
                "duration_us_calc": duration_ns / 1_000.0,
                "duration_ms": duration_ns / 1_000_000.0,

                "duration_us_logged": span_field_value(start_row, end_row, "duration_us"),

                "depth": span_field_value(start_row, end_row, "depth"),
                "row_size": span_field_value(start_row, end_row, "row_size"),
                "frontier_size": span_field_value(start_row, end_row, "frontier_size"),

                "attributes": attributes,
                "attributes_json": json.dumps(
                    attributes,
                    ensure_ascii=False,
                    sort_keys=True,
                    separators=(",", ":"),
                ),

                "start_event": start_row,
                "end_event": end_row,
            }
            for attribute_name, attribute_value in attributes.items():
                span[f"attr.{attribute_name}"] = attribute_value
            spans.append(span)

    unclosed_count = sum(len(v) for v in stacks.values())
    unmatched_end_count = len(unmatched_ends)

    if not spans:
        raise ValueError("No spans reconstructed. Make sure events use .start and .end suffixes.")

    span_df = pd.DataFrame(spans)
    span_df = span_df.sort_values(["start_ns", "end_ns"]).reset_index(drop=True)

    # Keep the raw logger thread identifier for reconstruction/grouping, but expose
    # a compact and stable zero-based identifier in every generated view.  Ordering
    # by first observed span makes the mapping deterministic for a given execution.
    thread_order = span_df["thread_id"].drop_duplicates().tolist()
    thread_indices = {thread_id: index for index, thread_id in enumerate(thread_order)}
    span_df["thread_index"] = span_df["thread_id"].map(thread_indices).astype(int)

    t0 = span_df["start_ns"].min()

    span_df["start_ms"] = (span_df["start_ns"] - t0) / 1_000_000.0
    span_df["end_ms"] = (span_df["end_ns"] - t0) / 1_000_000.0

    span_df.attrs["unclosed_count"] = unclosed_count
    span_df.attrs["unmatched_end_count"] = unmatched_end_count

    return span_df


# ============================================================
# Overlap lane assignment
# ============================================================

def assign_overlap_lanes(
        spans: pd.DataFrame,
        group_cols=("thread_id",),
) -> pd.DataFrame:
    spans = spans.copy()
    spans["overlap_lane"] = 0

    for _, group in spans.groupby(list(group_cols), dropna=False):
        lane_end_times = []
        group = group.sort_values(["start_ns", "end_ns"])

        for idx, row in group.iterrows():
            start = row["start_ns"]
            end = row["end_ns"]

            assigned_lane = None

            for lane_idx, lane_end in enumerate(lane_end_times):
                if lane_end <= start:
                    assigned_lane = lane_idx
                    lane_end_times[lane_idx] = end
                    break

            if assigned_lane is None:
                assigned_lane = len(lane_end_times)
                lane_end_times.append(end)

            spans.at[idx, "overlap_lane"] = assigned_lane

    return spans


# ============================================================
# Labeling
# ============================================================

def make_hover_text(row) -> str:
    overlap_lane = row.get("overlap_lane", 0)
    if is_missing_value(overlap_lane):
        overlap_lane = 0
    parts = [
        f"<b>{html.escape(str(row['name']))}</b>",
        f"thread: {int(row.get('thread_index', row['thread_id']))}",
        f"lane: {int(overlap_lane)}",
        f"start: {row['start_ms']:.3f} ms",
        f"end: {row['end_ms']:.3f} ms",
        f"duration: {row['duration_ms']:.3f} ms",
    ]

    if pd.notna(row.get("duration_us_logged")):
        parts.append(
            f"duration logged: {float(row['duration_us_logged']) / 1000.0:.3f} ms"
        )

    if pd.notna(row.get("depth")):
        parts.append(f"depth: {int(row['depth'])}")

    if pd.notna(row.get("row_size")):
        parts.append(f"row_size: {int(row['row_size'])}")

    if pd.notna(row.get("frontier_size")):
        parts.append(f"frontier_size: {int(row['frontier_size'])}")

    if pd.notna(row.get("seq_start")) and pd.notna(row.get("seq_end")):
        parts.append(f"seq: {int(row['seq_start'])} → {int(row['seq_end'])}")

    attributes = row.get("attributes")
    if isinstance(attributes, dict) and attributes:
        already_rendered = {"depth", "row_size", "frontier_size"}
        dynamic_attributes = [
            (key, value)
            for key, value in attributes.items()
            if key not in already_rendered
        ]
        if dynamic_attributes:
            parts.append("<b>attributes</b>")
            for key, value in sorted(dynamic_attributes, key=lambda item: _attribute_sort_key(item[0])):
                safe_key = html.escape(str(key))
                safe_value = html.escape(format_attribute_value(value))
                parts.append(f"{safe_key}: {safe_value}")

    return "<br>".join(parts)


def display_thread_index(row) -> int:
    value = row.get("thread_index", row["thread_id"])
    return int(value)


def make_lane_by_thread(row) -> str:
    return f"thread {display_thread_index(row)} / lane {int(row['overlap_lane'])}"


# ============================================================
# Figure builders
# ============================================================

def build_timeline_figure(
        spans: pd.DataFrame,
        title: str,
        group_cols,
        lane_fn,
) -> go.Figure:
    df = spans.copy()
    df = assign_overlap_lanes(df, group_cols=group_cols)

    df["lane"] = df.apply(lane_fn, axis=1)
    df["hover"] = df.apply(make_hover_text, axis=1)

    df = df.sort_values(["start_ns", "end_ns"])

    lane_order = (
        df[["lane"]]
        .drop_duplicates()
        ["lane"]
        .tolist()
    )

    lane_order = list(reversed(lane_order))

    fig = go.Figure()

    for event_name, group in df.groupby("name", sort=False):
        customdata = [
            [hover, attributes_json]
            for hover, attributes_json in zip(
                group["hover"].tolist(),
                group["attributes_json"].tolist(),
            )
        ]
        fig.add_trace(go.Bar(
            x=group["duration_ms"],
            y=group["lane"],
            base=group["start_ms"],
            orientation="h",
            name=event_name,
            customdata=customdata,
            hovertemplate="%{customdata[0]}<extra></extra>",
        ))

    fig.update_layout(
        title=title,
        xaxis_title="Time since first span [ms]",
        yaxis_title="Execution lane",
        barmode="overlay",
        bargap=0.20,
        height=max(550, 80 + 42 * len(lane_order)),
        legend_title="Event type",
        yaxis={
            "categoryorder": "array",
            "categoryarray": lane_order,
        },
        margin={
            "l": 280,
            "r": 40,
            "t": 80,
            "b": 60,
        },
    )

    return fig


def build_run_overview_html(run: dict) -> str:
    spans = run["spans"]
    events = run["events"]

    total_time_ms = spans["end_ms"].max() - spans["start_ms"].min()
    span_count = len(spans)
    event_count = len(events)
    event_types = spans["name"].nunique()
    threads = spans["thread_id"].nunique()

    unclosed_count = spans.attrs.get("unclosed_count", 0)
    unmatched_end_count = spans.attrs.get("unmatched_end_count", 0)

    ranked = (
        spans
        .groupby("name", as_index=False)
        .agg(total_ms=("duration_ms", "sum"), count=("duration_ms", "count"))
        .sort_values("total_ms", ascending=False)
        .reset_index(drop=True)
    )

    rows = "\n".join(
        f"""
        <tr data-overview-event-row data-rank="{rank + 1}" data-event-name="{html.escape(str(r['name']))}"{' hidden' if rank >= 10 else ''}>
          <td>{rank + 1}</td>
          <td>{html.escape(str(r["name"]))}</td>
          <td>{r["total_ms"]:.3f}</td>
          <td>{int(r["count"])}</td>
        </tr>
        """
        for rank, (_, r) in enumerate(ranked.iterrows())
    )

    return f"""
    <div class="overview-card" data-overview-card data-overview-expanded="false">
      <h2>Run overview</h2>

      <table class="meta-table">
        <tr><th>Log file</th><td>{html.escape(str(run["relative_path"]))}</td></tr>
        <tr><th>Events</th><td>{event_count}</td></tr>
        <tr><th>Spans</th><td>{span_count}</td></tr>
        <tr><th>Event types</th><td>{event_types}</td></tr>
        <tr><th>Threads</th><td>{threads}</td></tr>
        <tr><th>Total window</th><td>{total_time_ms:.3f} ms</td></tr>
        <tr><th>Unclosed starts</th><td>{unclosed_count}</td></tr>
        <tr><th>Unmatched ends</th><td>{unmatched_end_count}</td></tr>
      </table>

      <div class="overview-event-heading">
        <div>
          <h3>Event types by total time</h3>
          <span data-overview-status>Showing top {min(10, len(ranked))} of {len(ranked)}</span>
        </div>
        <div class="overview-event-actions">
          <input type="search" placeholder="Find event type..." aria-label="Find event type" data-overview-search />
          <button type="button" data-overview-toggle>Show all {len(ranked)}</button>
        </div>
      </div>

      <table class="meta-table overview-event-table">
        <thead>
          <tr>
            <th>#</th>
            <th>Event type</th>
            <th>Total ms</th>
            <th>Count</th>
          </tr>
        </thead>
        <tbody>{rows}</tbody>
      </table>
    </div>
    """


def _merged_interval_duration_ms(intervals: list[tuple[float, float]]) -> float:
    """Return wall-clock coverage of possibly nested/overlapping span intervals."""
    if not intervals:
        return 0.0
    ordered = sorted(intervals)
    current_start, current_end = ordered[0]
    total = 0.0
    for start, end in ordered[1:]:
        if start <= current_end:
            current_end = max(current_end, end)
            continue
        total += current_end - current_start
        current_start, current_end = start, end
    return total + (current_end - current_start)


def _comparison_source_kind(run: dict) -> str:
    """Classify an input log for comparison defaults.

    The HTTP benchmark emits both per-request event files and one long-lived
    ``server.events.jsonl``. The latter spans server startup and many requests, so
    treating it as another BFS repetition badly distorts cross-configuration
    distributions. It remains available, but is disabled by default in the UI.
    """
    if bool(run.get("is_average", False)):
        return "average"

    relative = Path(str(run.get("relative_path", run.get("label", ""))))
    parts = tuple(part.lower() for part in relative.parts)
    name = relative.name.lower()
    if name == "server.events.jsonl":
        return "server"
    if "requests" in parts and name.endswith(".events.jsonl"):
        return "request"
    if name.endswith(".events.jsonl"):
        return "event"
    return "other"


def _comparison_group_name(run: dict) -> str:
    """Return the benchmark/run configuration represented by one event file.

    For cache benchmarks this turns paths such as
    ``08-no-cache/events/requests/r03.events.jsonl`` into ``08-no-cache``.
    When the dashboard input itself is one run directory, the absolute log path
    supplies the component immediately before ``events``.
    """
    explicit = str(run.get("comparison_group", "")).strip()
    if explicit:
        return explicit

    candidates = [Path(str(run.get("relative_path", "")))]
    log_file = run.get("log_file")
    if log_file:
        candidates.append(Path(log_file))

    for candidate in candidates:
        parts = candidate.parts
        lowered = [part.lower() for part in parts]
        if "events" in lowered:
            index = len(lowered) - 1 - lowered[::-1].index("events")
            if index > 0:
                return parts[index - 1]

    label = str(run.get("label", "")).strip()
    if label.startswith("Average — "):
        label = label[len("Average — "):].strip()
    if label:
        normalized = label.replace("\\", "/")
        return normalized.split("/", 1)[0]
    return "run"


def _comparison_metric_options(
        families: list[dict[str, Any]],
        events: list[dict[str, Any]],
) -> tuple[list[dict[str, str]], str]:
    options: list[dict[str, str]] = []
    family_names = {str(item["name"]) for item in families}
    event_names = {str(item["name"]) for item in events}

    for name in sorted(family_names, key=lambda value: value.lower()):
        options.append({
            "key": f"family:{name}",
            "kind": "family",
            "name": name,
            "label": comparison_family_label(name),
        })
    for name in sorted(event_names, key=lambda value: value.lower()):
        options.append({
            "key": f"event:{name}",
            "kind": "event",
            "name": name,
            "label": name,
        })

    preferred = (
        "event:full_bfs",
        "family:full_bfs",
        "event:bfs",
        "family:bfs",
    )
    keys = {option["key"] for option in options}
    default_metric = next((key for key in preferred if key in keys), options[0]["key"] if options else "")
    return options, default_metric


def comparison_family_label(name: str) -> str:
    labels = {
        "full_bfs": "Total BFS",
        "bfs": "BFS",
        "neo4j": "Neo4j",
        "arango": "ArangoDB",
        "cugraph": "GPU / cuGraph",
        "gpu": "GPU",
        "redis": "Redis",
        "mmap": "MMAP",
        "postgresql": "PostgreSQL",
        "backend": "Backend scheduler",
    }
    return labels.get(name, name)


def build_comparison_metadata(runs: list[dict]) -> dict[str, Any]:
    """Build per-sample metrics grouped by benchmark/run configuration.

    A *sample* is one JSONL execution (normally one request/repetition). The UI
    aggregates these samples by configuration, so a box represents e.g.
    ``08-no-cache`` rather than an event family or a raw JSONL filename.

    Durations remain interval unions rather than naive sums. This avoids double
    counting nested instrumentation inside one event family.
    """
    family_totals: dict[str, float] = defaultdict(float)
    family_counts: dict[str, int] = defaultdict(int)
    event_totals: dict[str, float] = defaultdict(float)
    event_counts: dict[str, int] = defaultdict(int)
    run_rows: list[dict[str, Any]] = []

    for run in runs:
        spans = run["spans"]
        family_intervals: dict[str, list[tuple[float, float]]] = defaultdict(list)
        event_intervals: dict[str, list[tuple[float, float]]] = defaultdict(list)
        event_occurrences: dict[str, int] = defaultdict(int)
        family_occurrences: dict[str, int] = defaultdict(int)

        for _, span in spans.iterrows():
            event_name = str(span["name"])
            family = event_name_prefix(event_name)
            interval = (float(span["start_ms"]), float(span["end_ms"]))
            family_intervals[family].append(interval)
            event_intervals[event_name].append(interval)
            family_occurrences[family] += 1
            event_occurrences[event_name] += 1

        groups = {
            family: _merged_interval_duration_ms(intervals)
            for family, intervals in family_intervals.items()
        }
        events = {
            event_name: _merged_interval_duration_ms(intervals)
            for event_name, intervals in event_intervals.items()
        }

        for family, duration in groups.items():
            family_totals[family] += duration
            family_counts[family] += family_occurrences[family]
        for event_name, duration in events.items():
            event_totals[event_name] += duration
            event_counts[event_name] += event_occurrences[event_name]

        run_rows.append({
            "id": str(run["safe_id"]),
            "label": str(run["label"]),
            "group": _comparison_group_name(run),
            "sourceKind": _comparison_source_kind(run),
            "isAverage": bool(run.get("is_average", False)),
            "groups": groups,
            "events": events,
        })

    families = [
        {
            "name": name,
            "label": comparison_family_label(name),
            "runCount": sum(1 for run in run_rows if name in run["groups"]),
            "spanCount": family_counts[name],
            "totalMs": family_totals[name],
        }
        for name in sorted(family_totals, key=lambda name: (-family_totals[name], name.lower()))
    ]
    events = [
        {
            "name": name,
            "family": event_name_prefix(name),
            "runCount": sum(1 for run in run_rows if name in run["events"]),
            "spanCount": event_counts[name],
            "totalMs": event_totals[name],
        }
        for name in sorted(event_totals, key=lambda name: (-event_totals[name], name.lower()))
    ]

    group_names = sorted({run["group"] for run in run_rows}, key=lambda value: value.lower())
    groups = []
    for name in group_names:
        samples = [run for run in run_rows if run["group"] == name]
        groups.append({
            "name": name,
            "sampleCount": len(samples),
            "defaultSampleCount": sum(
                run["sourceKind"] not in {"server", "average"}
                for run in samples
            ),
        })

    source_labels = {
        "request": "Per-request event logs",
        "event": "Other .events.jsonl logs",
        "server": "server.events.jsonl",
        "average": "Generated average executions",
        "other": "Other JSONL logs",
    }
    source_order = ("request", "event", "other", "server", "average")
    sources = [
        {
            "name": source,
            "label": source_labels[source],
            "count": sum(run["sourceKind"] == source for run in run_rows),
            "defaultEnabled": source not in {"server", "average"},
        }
        for source in source_order
        if any(run["sourceKind"] == source for run in run_rows)
    ]

    metrics, default_metric = _comparison_metric_options(families, events)
    return {
        "runs": run_rows,
        "runGroups": groups,
        "sources": sources,
        "families": families,
        "events": events,
        "metrics": metrics,
        "defaultMetric": default_metric,
    }


def build_comparison_view(runs: list[dict]) -> str:
    metadata = build_comparison_metadata(runs)

    group_controls = []
    for index, group in enumerate(metadata["runGroups"]):
        group_controls.append(
            f"""<label class="comparison-check-row" for="comparison_group_{index}">
                 <input id="comparison_group_{index}" type="checkbox" data-comparison-group
                        value="{html.escape(group['name'])}" checked />
                 <span>{html.escape(group['name'])}</span><small>{int(group['defaultSampleCount'])} samples</small>
               </label>"""
        )

    source_controls = []
    for index, source in enumerate(metadata["sources"]):
        source_controls.append(
            f"""<label class="comparison-check-row" for="comparison_source_{index}">
                 <input id="comparison_source_{index}" type="checkbox" data-comparison-source
                        value="{html.escape(source['name'])}" {'checked' if source['defaultEnabled'] else ''} />
                 <span>{html.escape(source['label'])}</span><small>{int(source['count'])}</small>
               </label>"""
        )

    default_backend_families = {
        "neo4j", "arango", "cugraph", "gpu", "redis", "mmap", "postgresql", "backend"
    }
    available_default_backends = {
        family["name"] for family in metadata["families"]
        if family["name"] in default_backend_families
    }
    if not available_default_backends:
        available_default_backends = {
            family["name"] for family in metadata["families"][: min(5, len(metadata["families"]))]
        }

    family_controls = []
    for index, family in enumerate(metadata["families"]):
        family_controls.append(
            f"""<label class="comparison-check-row" for="comparison_family_{index}">
                 <input id="comparison_family_{index}" type="checkbox" data-comparison-family
                        value="{html.escape(family['name'])}" {'checked' if family['name'] in available_default_backends else ''} />
                 <span>{html.escape(str(family['label']))}</span><small>{int(family['runCount'])} samples</small>
               </label>"""
        )

    # Event detail is opt-in. A small top set is preselected so enabling the panel
    # produces something useful without creating an unreadable 20-trace legend.
    event_controls = []
    for index, event in enumerate(metadata["events"]):
        event_controls.append(
            f"""<label class="comparison-check-row" for="comparison_event_{index}" data-comparison-event-row
                       data-event-search="{html.escape(event['name'].lower())}">
                 <input id="comparison_event_{index}" type="checkbox" data-comparison-event
                        value="{html.escape(event['name'])}" {'checked' if index < 6 else ''} />
                 <span>{html.escape(event['name'])}</span><small>{float(event['totalMs']):.1f} ms</small>
               </label>"""
        )

    family_metric_options = "".join(
        f'<option value="{html.escape(option["key"])}" '
        f'{"selected" if option["key"] == metadata["defaultMetric"] else ""}>'
        f'{html.escape(option["label"])}</option>'
        for option in metadata["metrics"]
        if option["kind"] == "family"
    )
    event_metric_options = "".join(
        f'<option value="{html.escape(option["key"])}" '
        f'{"selected" if option["key"] == metadata["defaultMetric"] else ""}>'
        f'{html.escape(option["label"])}</option>'
        for option in metadata["metrics"]
        if option["kind"] == "event"
    )

    return f"""
    <div class="comparison-workspace" data-comparison-workspace>
      <div class="comparison-intro">
        <div>
          <h2>Compare run configurations</h2>
          <p>Each distribution represents one benchmark configuration (for example <code>08-no-cache</code>) across its repetitions/requests. Choose one BFS/event metric for the main comparison, then optionally add backend and event-detail panels.</p>
        </div>
        <div class="comparison-view-switches" aria-label="Comparison charts">
          <label><input type="checkbox" data-comparison-chart-toggle value="boxplot" checked /> Distribution + summary</label>
          <label><input type="checkbox" data-comparison-chart-toggle value="families" checked /> Backend breakdown</label>
          <label><input type="checkbox" data-comparison-chart-toggle value="events" /> Event detail</label>
        </div>
      </div>

      <div class="comparison-layout">
        <aside class="comparison-filters">
          <section class="comparison-primary-filter">
            <label class="comparison-select-label">
              <strong>Primary metric</strong>
              <span>One metric; one box per run configuration.</span>
              <select data-comparison-primary-metric>
                <optgroup label="Event families">{family_metric_options}</optgroup>
                <optgroup label="Individual events">{event_metric_options}</optgroup>
              </select>
            </label>
            <div class="comparison-inline-selects">
              <label>Run order
                <select data-comparison-sort>
                  <option value="name">Configured/name</option>
                  <option value="median">Fastest median</option>
                </select>
              </label>
              <label>Y scale
                <select data-comparison-scale>
                  <option value="linear">Linear</option>
                  <option value="log">Logarithmic</option>
                </select>
              </label>
            </div>
          </section>

          <section>
            <div class="comparison-filter-heading"><strong>Run configurations</strong><div><button type="button" data-comparison-groups="all">All</button><button type="button" data-comparison-groups="none">None</button></div></div>
            <div class="comparison-check-list comparison-run-list">{''.join(group_controls)}</div>
          </section>

          <section>
            <div class="comparison-filter-heading"><strong>Sample sources</strong></div>
            <p class="comparison-filter-note"><code>server.events.jsonl</code> is intentionally off by default because it covers server lifetime/startup, not one BFS repetition.</p>
            <div class="comparison-check-list">{''.join(source_controls)}</div>
          </section>

          <section>
            <div class="comparison-filter-heading"><strong>Backend/event families</strong><div><button type="button" data-comparison-families="runtime">Runtime defaults</button><button type="button" data-comparison-families="all">All</button><button type="button" data-comparison-families="none">None</button></div></div>
            <p class="comparison-filter-note">Controls the optional backend breakdown only. Startup/load families remain available but are not selected automatically.</p>
            <div class="comparison-check-list">{''.join(family_controls)}</div>
          </section>

          <section>
            <div class="comparison-filter-heading"><strong>Individual events</strong><div><button type="button" data-comparison-events="top">Top 6</button><button type="button" data-comparison-events="all">All</button><button type="button" data-comparison-events="none">None</button></div></div>
            <input class="comparison-event-search" type="search" placeholder="Filter event names..." data-comparison-event-search />
            <div class="comparison-check-list comparison-event-list">{''.join(event_controls)}</div>
          </section>
        </aside>

        <div class="comparison-charts">
          <div class="comparison-status" data-comparison-status></div>
          <section class="comparison-chart-card" data-comparison-chart="boxplot">
            <div class="comparison-chart-heading">
              <div><h3><span data-comparison-primary-label>Selected metric</span> by run configuration</h3><p>Every box contains that metric from the selected repetitions/request logs of one configuration. Lower is faster.</p></div>
            </div>
            <div id="comparison_boxplot" class="comparison-plot comparison-primary-plot"></div>
            <div class="comparison-summary-wrap">
              <table class="comparison-summary-table">
                <thead><tr><th>Run configuration</th><th>n</th><th>Median</th><th>IQR</th><th>Min</th><th>Max</th><th>vs best</th></tr></thead>
                <tbody data-comparison-summary-body></tbody>
              </table>
            </div>
          </section>
          <section class="comparison-chart-card" data-comparison-chart="families">
            <div class="comparison-chart-heading"><div><h3>Backend execution time by run configuration</h3><p>Bars are medians across the same selected samples; whiskers show the interquartile range. Toggle Neo4j, ArangoDB, GPU/cuGraph, Redis and other families at left.</p></div></div>
            <div id="comparison_families" class="comparison-plot"></div>
          </section>
          <section class="comparison-chart-card" data-comparison-chart="events" hidden>
            <div class="comparison-chart-heading"><div><h3>Selected event time by run configuration</h3><p>Individual event medians + IQR, aggregated by configuration instead of plotting every raw JSONL filename.</p></div></div>
            <div id="comparison_events" class="comparison-plot"></div>
          </section>
        </div>
      </div>

      <script type="application/json" id="comparison_metadata">{safe_json_for_html(metadata)}</script>
    </div>
    """

# ============================================================
# Dashboard generation
# ============================================================

VIEW_OPTIONS = [
    ("overview", "Overview"),
    ("timeline_thread", "Timeline: by thread"),
    ("comparison", "Compare runs"),
]


def figure_to_div(fig: go.Figure, div_id: str) -> str:
    return fig.to_html(
        full_html=False,
        include_plotlyjs=False,
        div_id=div_id,
        config={
            "responsive": True,
            "displaylogo": False,
        },
    )


def build_timeline_thread_view(run: dict) -> str:
    spans = run["spans"]
    run_id = run["safe_id"]
    plot_id = f"{run_id}_timeline_thread"
    metadata_id = f"{run_id}_timeline_thread_filters"
    metadata = build_timeline_filter_metadata(spans)

    prefix_controls = []
    for index, prefix in enumerate(metadata["prefixes"]):
        checkbox_id = f"{run_id}_prefix_{index}"
        prefix_controls.append(
            f'''<label class="filter-chip" for="{html.escape(checkbox_id)}">
                 <input id="{html.escape(checkbox_id)}" type="checkbox"
                        data-timeline-prefix value="{html.escape(prefix['name'])}" checked />
                 <span>{html.escape(prefix['name'])}</span>
                 <small>{int(prefix['count'])}</small>
               </label>'''
        )

    attribute_options = [
        '<option value="">Choose an attribute...</option>'
    ]
    for attribute_name in metadata["attributes"]:
        attribute_options.append(
            f'<option value="{html.escape(attribute_name)}">{html.escape(attribute_name)}</option>'
        )

    duration_event_options = [
        '<option value="">Choose an event type...</option>'
    ]
    for event in metadata["events"]:
        duration_event_options.append(
            '<option '
            f'value="{html.escape(str(event["name"]))}" '
            f'data-min-duration-ms="{float(event["minDurationMs"]):.12g}" '
            f'data-max-duration-ms="{float(event["maxDurationMs"]):.12g}">'
            f'{html.escape(str(event["name"]))}'
            '</option>'
        )

    figure = build_timeline_figure(
        spans,
        title=f"Execution Timeline by Thread — {run['label']}",
        group_cols=("thread_id",),
        lane_fn=make_lane_by_thread,
    )

    return f"""
    <div class="timeline-filtered-view"
         data-timeline-filter
         data-plot-id="{html.escape(plot_id)}"
         data-metadata-id="{html.escape(metadata_id)}">
      <div class="timeline-filter-panel">
        <div class="timeline-filter-section">
          <div class="timeline-filter-heading">
            <div>
              <strong>Event families</strong>
              <span>Toggle every event sharing the first name component.</span>
            </div>
            <div class="filter-actions">
              <button type="button" data-prefix-action="all">Show all</button>
              <button type="button" data-prefix-action="none">Hide all</button>
            </div>
          </div>
          <div class="filter-chip-list">{''.join(prefix_controls)}</div>
        </div>

        <div class="timeline-filter-section">
          <div class="timeline-filter-heading">
            <div>
              <strong>Event attributes</strong>
              <span>Values within one attribute use OR; multiple attribute cards combine with AND.</span>
            </div>
            <button type="button" data-filter-reset>Reset filters</button>
          </div>
          <div class="attribute-filter-toolbar">
            <label class="attribute-select-label">
              Add attribute
              <select data-attribute-select>{''.join(attribute_options)}</select>
            </label>
            <button type="button" data-add-attribute-filter disabled>Add filter</button>
          </div>
          <div class="attribute-filter-card-list" data-active-attribute-filters>
            <span class="filter-empty-hint">No attribute filters active.</span>
          </div>
        </div>

        <div class="timeline-filter-section">
          <div class="timeline-filter-heading">
            <div>
              <strong>Event duration ranges</strong>
              <span>Apply an inclusive minimum and/or maximum duration to a selected event type. Other event types remain unchanged.</span>
            </div>
          </div>
          <div class="duration-filter-toolbar">
            <label class="duration-select-label">
              Add event
              <select data-duration-event-select>{''.join(duration_event_options)}</select>
            </label>
            <button type="button" data-add-duration-filter disabled>Add range</button>
          </div>
          <div class="duration-filter-card-list" data-active-duration-filters>
            <span class="filter-empty-hint">No duration ranges active.</span>
          </div>
        </div>

        <div class="timeline-filter-status" data-filter-status></div>
      </div>
      <div class="timeline-plot">{figure_to_div(figure, div_id=plot_id)}</div>
      <script type="application/json" id="{html.escape(metadata_id)}">
        {safe_json_for_html(metadata)}
      </script>
    </div>
    """


def build_dashboard(
        runs: list[dict],
        output_html: Path,
        inline_plotly: bool = False,
        average_index: Path | None = None,
):
    if inline_plotly:
        from plotly.offline import get_plotlyjs
        plotly_script = f"<script>{get_plotlyjs()}</script>"
    else:
        plotly_script = (
            '<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>'
        )

    run_options = "\n".join(
        f'<option value="{html.escape(run["safe_id"])}">{html.escape(run["label"])}</option>'
        for run in runs
    )

    view_options = "\n".join(
        f'<option value="{value}">{label}</option>'
        for value, label in VIEW_OPTIONS
    )

    average_index_link = ""
    if average_index is not None and average_index.is_file():
        average_index_href = average_index.relative_to(output_html.parent).as_posix()
        average_index_link = (
            f'<a class="average-index-link" href="{html.escape(average_index_href)}">'
            "Open average report index</a>"
        )

    panels = []

    for run in runs:
        run_id = run["safe_id"]
        spans = run["spans"]

        view_html = {
            "overview": build_run_overview_html(run),
            "timeline_thread": build_timeline_thread_view(run),
        }

        for view_id in ("overview", "timeline_thread"):
            panels.append(f"""
            <section
              class="chart-panel"
              data-run="{html.escape(run_id)}"
              data-view="{html.escape(view_id)}"
            >
              {view_html[view_id]}
            </section>
            """)

    panels.append(f"""
    <section class="chart-panel comparison-panel" data-view="comparison">
      {build_comparison_view(runs)}
    </section>
    """)

    panels_html = "\n".join(panels)

    css = """
    <style>
      :root {
        --bg: #0f172a;
        --panel: #111827;
        --panel2: #1f2937;
        --text: #e5e7eb;
        --muted: #9ca3af;
        --border: #374151;
        --accent: #60a5fa;
      }

      body {
        margin: 0;
        font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
        background: var(--bg);
        color: var(--text);
      }

      header {
        position: sticky;
        top: 0;
        z-index: 10;
        background: rgba(15, 23, 42, 0.96);
        border-bottom: 1px solid var(--border);
        padding: 16px 24px;
        backdrop-filter: blur(8px);
      }

      h1 {
        margin: 0 0 12px 0;
        font-size: 22px;
      }

      .controls {
        display: flex;
        gap: 16px;
        flex-wrap: wrap;
        align-items: end;
      }

      label {
        display: flex;
        flex-direction: column;
        gap: 6px;
        color: var(--muted);
        font-size: 13px;
      }

      select {
        min-width: 280px;
        padding: 8px 10px;
        border-radius: 8px;
        border: 1px solid var(--border);
        background: var(--panel2);
        color: var(--text);
      }

      main {
        padding: 24px;
      }

      .chart-panel {
        display: none;
        background: white;
        color: black;
        border-radius: 12px;
        padding: 12px;
        overflow-x: auto;
      }

      .chart-panel.active {
        display: block;
      }

      .overview-card {
        background: white;
        color: #111827;
        border-radius: 12px;
        padding: 24px;
      }

      .overview-card h2,
      .overview-card h3 {
        margin-top: 0;
      }

      .meta-table {
        border-collapse: collapse;
        min-width: 640px;
        margin-bottom: 24px;
      }

      .meta-table th,
      .meta-table td {
        text-align: left;
        border: 1px solid #d1d5db;
        padding: 8px 12px;
      }

      .meta-table th {
        background: #f3f4f6;
      }

      .hint {
        color: var(--muted);
        margin-top: 10px;
        font-size: 13px;
      }

      .average-index-link {
        display: inline-flex;
        align-items: center;
        min-height: 38px;
        padding: 8px 13px;
        border-radius: 9px;
        text-decoration: none;
        font-weight: 700;
        background: #2563eb;
        color: white;
      }

.average-profile-heading {
        display: flex;
        justify-content: space-between;
        align-items: start;
        gap: 20px;
        flex-wrap: wrap;
      }

.average-cards {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
        gap: 12px;
        margin: 18px 0;
      }

.average-card span {
        display: block;
        color: #64748b;
        font-size: 12px;
        text-transform: uppercase;
        letter-spacing: 0.06em;
      }

.average-chart {
        overflow-x: auto;
        background: white;
        border-radius: 12px;
        border: 1px solid #e2e8f0;
      }

.average-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 13px;
      }

.average-table th:first-child,
      .average-table td:first-child {
        text-align: left;
      }

.timeline-filtered-view {
        background: #f8fafc;
        color: #111827;
        border-radius: 12px;
        overflow: hidden;
      }

      .timeline-filter-panel {
        display: grid;
        gap: 14px;
        padding: 18px 20px 10px;
        border-bottom: 1px solid #dbe3ef;
        background: linear-gradient(180deg, #f8fafc 0%, #eef2f7 100%);
      }

      .timeline-filter-section {
        padding: 14px;
        border: 1px solid #dbe3ef;
        border-radius: 12px;
        background: white;
      }

      .timeline-filter-heading {
        display: flex;
        align-items: start;
        justify-content: space-between;
        gap: 16px;
        flex-wrap: wrap;
        margin-bottom: 11px;
      }

      .timeline-filter-heading strong,
      .timeline-filter-heading span {
        display: block;
      }

      .timeline-filter-heading span {
        margin-top: 3px;
        color: #64748b;
        font-size: 12px;
      }

      .filter-actions {
        display: flex;
        gap: 7px;
      }

      .timeline-filter-panel button {
        padding: 7px 10px;
        border: 1px solid #cbd5e1;
        border-radius: 8px;
        background: #f8fafc;
        color: #0f172a;
        cursor: pointer;
        font-weight: 650;
      }

      .timeline-filter-panel button:hover {
        background: #e2e8f0;
      }

      .filter-chip-list {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
      }

      .filter-chip {
        display: inline-flex;
        flex-direction: row;
        align-items: center;
        gap: 7px;
        padding: 7px 10px;
        border: 1px solid #cbd5e1;
        border-radius: 999px;
        background: #f8fafc;
        color: #0f172a;
        cursor: pointer;
        user-select: none;
      }

      .filter-chip:has(input:checked) {
        border-color: #60a5fa;
        background: #dbeafe;
      }

      .filter-chip input {
        margin: 0;
        accent-color: #2563eb;
      }

      .filter-chip small {
        min-width: 22px;
        padding: 1px 6px;
        border-radius: 999px;
        background: rgba(15, 23, 42, 0.09);
        color: #475569;
        text-align: center;
      }

      .attribute-filter-toolbar,
      .duration-filter-toolbar {
        display: flex;
        align-items: end;
        gap: 10px;
        flex-wrap: wrap;
      }

      .attribute-filter-toolbar > button:disabled,
      .duration-filter-toolbar > button:disabled {
        cursor: not-allowed;
        opacity: 0.5;
      }

      .attribute-select-label,
      .duration-select-label {
        display: grid;
        grid-template-columns: max-content minmax(220px, 420px);
        align-items: center;
        gap: 10px;
        color: #475569;
        font-weight: 650;
      }

      .attribute-select-label select,
      .duration-select-label select {
        min-width: 0;
        width: 100%;
        background: white;
        color: #111827;
        border-color: #cbd5e1;
      }

      .attribute-filter-card-list,
      .duration-filter-card-list {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(min(100%, 360px), 1fr));
        gap: 12px;
        margin-top: 14px;
      }

      .attribute-filter-card,
      .duration-filter-card {
        min-width: 0;
        padding: 12px;
        border: 1px solid #cbd5e1;
        border-radius: 12px;
        background: #ffffff;
        box-shadow: 0 1px 2px rgba(15, 23, 42, 0.05);
      }

      .attribute-filter-card-heading {
        display: flex;
        align-items: flex-start;
        justify-content: space-between;
        gap: 12px;
        margin-bottom: 10px;
      }

      .attribute-filter-card-title {
        display: grid;
        gap: 2px;
        min-width: 0;
      }

      .attribute-filter-card-title strong {
        color: #0f172a;
        overflow-wrap: anywhere;
      }

      .attribute-filter-card-title span {
        color: #64748b;
        font-size: 12px;
      }

      .attribute-filter-card-actions {
        display: flex;
        align-items: center;
        gap: 6px;
        flex-shrink: 0;
      }

      .attribute-filter-card-actions button {
        padding: 5px 8px;
        font-size: 12px;
      }

      .attribute-filter-card-actions .attribute-filter-remove {
        width: 28px;
        height: 28px;
        padding: 0;
        border-color: #fecaca;
        background: #fff1f2;
        color: #be123c;
        font-size: 18px;
        line-height: 1;
      }

      .attribute-filter-card-actions .attribute-filter-remove:hover {
        background: #ffe4e6;
      }

      .duration-range-fields {
        display: grid;
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 10px;
      }

      .duration-range-field {
        display: grid;
        gap: 5px;
        color: #475569;
        font-size: 12px;
        font-weight: 650;
      }

      .duration-range-field input {
        min-width: 0;
        width: 100%;
        padding: 8px 9px;
        border: 1px solid #cbd5e1;
        border-radius: 8px;
        background: white;
        color: #111827;
      }

      .duration-range-field input:focus {
        outline: 2px solid rgba(37, 99, 235, 0.22);
        border-color: #60a5fa;
      }

      .duration-range-summary {
        margin-top: 9px;
        color: #64748b;
        font-size: 12px;
      }

      .duration-filter-card.range-invalid {
        border-color: #fca5a5;
        background: #fff7f7;
      }

      .duration-filter-card.range-invalid .duration-range-summary {
        color: #b91c1c;
        font-weight: 700;
      }

      .attribute-filter-card > .filter-chip-list {
        max-height: 190px;
        overflow-y: auto;
        padding-right: 3px;
      }

      .filter-empty-hint {
        color: #64748b;
        font-size: 13px;
      }

      .timeline-filter-status {
        color: #475569;
        font-size: 12px;
        font-weight: 650;
      }

      .timeline-plot {
        overflow-x: auto;
        background: white;
      }

.attributes-heading {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 18px;
        flex-wrap: wrap;
        margin-bottom: 16px;
      }

.attribute-filter {
        min-width: min(420px, 100%);
        padding: 10px 12px;
        border: 1px solid #cbd5e1;
        border-radius: 9px;
        background: white;
        color: #111827;
      }

.attribute-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 13px;
      }

.attribute-table thead {
        position: sticky;
        top: 0;
        background: #eef2f7;
      }


      .overview-event-heading {
        display: flex;
        align-items: end;
        justify-content: space-between;
        gap: 16px;
        flex-wrap: wrap;
        margin: 24px 0 10px;
      }

      .overview-event-heading h3 { margin: 0; }
      .overview-event-heading span { display: block; margin-top: 4px; color: #64748b; font-size: 12px; }
      .overview-event-actions { display: flex; gap: 8px; flex-wrap: wrap; }
      .overview-event-actions input {
        min-width: 240px;
        padding: 8px 10px;
        border: 1px solid #cbd5e1;
        border-radius: 8px;
        color: #111827;
        background: white;
      }
      .overview-event-actions button,
      .comparison-filter-heading button {
        padding: 7px 10px;
        border: 1px solid #cbd5e1;
        border-radius: 8px;
        background: #f8fafc;
        color: #0f172a;
        cursor: pointer;
        font-weight: 650;
      }
      .overview-event-table tr[hidden] { display: none; }

      .comparison-panel { background: #f8fafc; color: #0f172a; padding: 0; overflow: visible; }
      .comparison-workspace { min-width: 0; }
      .comparison-intro {
        display: flex;
        align-items: start;
        justify-content: space-between;
        gap: 18px;
        flex-wrap: wrap;
        padding: 20px;
        border-bottom: 1px solid #dbe3ef;
        background: white;
      }
      .comparison-intro h2 { margin: 0 0 6px; }
      .comparison-intro p { margin: 0; max-width: 900px; color: #64748b; font-size: 13px; line-height: 1.45; }
      .comparison-intro code,
      .comparison-filter-note code { padding: 1px 4px; border-radius: 4px; background: #eef2f7; color: #334155; }
      .comparison-view-switches { display: flex; gap: 8px; flex-wrap: wrap; }
      .comparison-view-switches label {
        display: inline-flex;
        flex-direction: row;
        align-items: center;
        gap: 7px;
        min-height: 36px;
        padding: 7px 10px;
        border: 1px solid #cbd5e1;
        border-radius: 999px;
        background: #f8fafc;
        color: #0f172a;
        cursor: pointer;
        font-weight: 650;
      }
      .comparison-view-switches label:has(input:checked) { border-color: #60a5fa; background: #dbeafe; }
      .comparison-view-switches input,
      .comparison-check-row input { accent-color: #2563eb; }

      .comparison-layout {
        display: grid;
        grid-template-columns: minmax(290px, 350px) minmax(0, 1fr);
        gap: 16px;
        padding: 16px;
      }
      .comparison-filters { display: grid; align-content: start; gap: 12px; min-width: 0; }
      .comparison-filters section {
        min-width: 0;
        padding: 12px;
        border: 1px solid #dbe3ef;
        border-radius: 12px;
        background: white;
      }
      .comparison-primary-filter { display: grid; gap: 12px; }
      .comparison-select-label { display: grid; gap: 5px; color: #334155; }
      .comparison-select-label span,
      .comparison-filter-note { margin: 0; color: #64748b; font-size: 11px; line-height: 1.4; }
      .comparison-select-label select,
      .comparison-inline-selects select {
        width: 100%;
        box-sizing: border-box;
        padding: 8px 9px;
        border: 1px solid #cbd5e1;
        border-radius: 8px;
        background: white;
        color: #0f172a;
      }
      .comparison-inline-selects { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
      .comparison-inline-selects label { display: grid; gap: 4px; color: #64748b; font-size: 11px; }
      .comparison-filter-heading {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 8px;
        margin-bottom: 9px;
      }
      .comparison-filter-heading > div { display: flex; gap: 5px; flex-wrap: wrap; }
      .comparison-filter-heading button { padding: 5px 7px; font-size: 11px; }
      .comparison-check-list {
        display: grid;
        gap: 3px;
        max-height: 245px;
        overflow: auto;
        padding-right: 3px;
      }
      .comparison-run-list { max-height: 300px; }
      .comparison-event-list { max-height: 330px; }
      .comparison-check-row {
        display: grid;
        grid-template-columns: 18px minmax(0, 1fr) auto;
        align-items: center;
        gap: 7px;
        padding: 5px 6px;
        border-radius: 7px;
        color: #334155;
        cursor: pointer;
        font-size: 12px;
      }
      .comparison-check-row:hover { background: #f1f5f9; }
      .comparison-check-row span { min-width: 0; overflow-wrap: anywhere; }
      .comparison-check-row small { color: #94a3b8; font-size: 10px; white-space: nowrap; }
      .comparison-event-search {
        width: 100%;
        box-sizing: border-box;
        margin: 8px 0;
        padding: 8px 9px;
        border: 1px solid #cbd5e1;
        border-radius: 8px;
        background: white;
        color: #111827;
      }
      .comparison-charts { display: grid; gap: 14px; min-width: 0; align-content: start; }
      .comparison-status {
        padding: 10px 12px;
        border: 1px solid #bfdbfe;
        border-radius: 10px;
        background: #eff6ff;
        color: #1e3a8a;
        font-size: 12px;
        font-weight: 650;
      }
      .comparison-chart-card {
        min-width: 0;
        border: 1px solid #dbe3ef;
        border-radius: 12px;
        background: white;
        overflow: hidden;
      }
      .comparison-chart-card[hidden] { display: none; }
      .comparison-chart-heading { padding: 16px 18px 0; }
      .comparison-chart-heading h3 { margin: 0; }
      .comparison-chart-heading p { margin: 4px 0 0; color: #64748b; font-size: 12px; line-height: 1.4; }
      .comparison-plot { min-height: 430px; }
      .comparison-primary-plot { min-height: 470px; }
      .comparison-summary-wrap { overflow-x: auto; padding: 0 18px 18px; }
      .comparison-summary-table { width: 100%; min-width: 690px; border-collapse: collapse; font-size: 12px; }
      .comparison-summary-table th,
      .comparison-summary-table td { padding: 8px 9px; border-bottom: 1px solid #e2e8f0; text-align: right; white-space: nowrap; }
      .comparison-summary-table th:first-child,
      .comparison-summary-table td:first-child { text-align: left; }
      .comparison-summary-table thead th { color: #64748b; font-size: 10px; text-transform: uppercase; letter-spacing: .04em; }
      .comparison-summary-table tr[data-best="true"] td:first-child { font-weight: 800; color: #166534; }
      .comparison-summary-table .comparison-no-data { text-align: center; color: #94a3b8; padding: 18px; }

      @media (max-width: 1050px) {
        .comparison-layout { grid-template-columns: 1fr; }
        .comparison-filters { grid-template-columns: repeat(2, minmax(0, 1fr)); }
        .comparison-primary-filter { grid-column: 1 / -1; }
        .comparison-check-list { max-height: 210px; }
      }
      @media (max-width: 680px) {
        .comparison-filters { grid-template-columns: 1fr; }
        .comparison-primary-filter { grid-column: auto; }
        .comparison-inline-selects { grid-template-columns: 1fr; }
      }

</style>
    """

    js = """
    <script>
      const timelineFilterStates = new WeakMap();

      function filterAttributeTable(tableId, query) {
        const normalized = query.trim().toLowerCase();
        const table = document.getElementById(tableId);
        if (!table) return;
        table.querySelectorAll("tbody tr").forEach(row => {
          row.style.display = !normalized || row.textContent.toLowerCase().includes(normalized)
            ? ""
            : "none";
        });
      }

      function cloneTimelineValue(value) {
        if (typeof structuredClone === "function") {
          return structuredClone(value);
        }
        return JSON.parse(JSON.stringify(value));
      }

      function isTimelineSequence(value) {
        return Array.isArray(value) || ArrayBuffer.isView(value);
      }

      function timelineSequenceLength(value) {
        return isTimelineSequence(value) ? value.length : 0;
      }

      function filterTimelineSequence(value, keptIndices, pointCount) {
        if (!isTimelineSequence(value) || value.length !== pointCount) {
          return value;
        }
        // Plotly accepts normal arrays, so typed numeric arrays can safely be
        // converted while filtering. This also avoids retaining their buffers.
        return keptIndices.map(index => value[index]);
      }

      function timelineTracePointCount(trace) {
        for (const key of ["x", "y", "base", "customdata"]) {
          const length = timelineSequenceLength(trace[key]);
          if (length > 0) return length;
        }
        return 0;
      }

      function timelineEventPrefix(name) {
        const text = String(name ?? "");
        const dot = text.indexOf(".");
        return dot >= 0 ? text.slice(0, dot) : text;
      }

      function timelineAttributeToken(value) {
        if (typeof value === "boolean") return `boolean:${value}`;
        if (typeof value === "number") return `number:${JSON.stringify(value)}`;
        if (typeof value === "string") return `string:${value}`;
        return `json:${JSON.stringify(value)}`;
      }

      function timelinePointAttributes(customdata) {
        if (!Array.isArray(customdata) || typeof customdata[1] !== "string") return {};
        try {
          const parsed = JSON.parse(customdata[1]);
          return parsed && typeof parsed === "object" ? parsed : {};
        } catch (_) {
          return {};
        }
      }

      function activeTimelineAttributeNames(panel) {
        return new Set(
          [...panel.querySelectorAll("[data-attribute-filter-card]")]
            .map(card => card.dataset.attributeName)
        );
      }

      function renderEmptyAttributeFilterHint(panel) {
        const container = panel.querySelector("[data-active-attribute-filters]");
        if (!container || container.querySelector("[data-attribute-filter-card]")) return;
        container.replaceChildren();
        const hint = document.createElement("span");
        hint.className = "filter-empty-hint";
        hint.textContent = "No attribute filters active.";
        container.appendChild(hint);
      }

      function updateAttributeFilterControls(panel) {
        const select = panel.querySelector("[data-attribute-select]");
        const addButton = panel.querySelector("[data-add-attribute-filter]");
        const active = activeTimelineAttributeNames(panel);

        [...select.options].forEach(option => {
          option.disabled = option.value !== "" && active.has(option.value);
        });

        if (select.value && active.has(select.value)) {
          select.value = "";
        }
        addButton.disabled = !select.value;
      }

      function activeTimelineDurationEventNames(panel) {
        return new Set(
          [...panel.querySelectorAll("[data-duration-filter-card]")]
            .map(card => card.dataset.eventName)
        );
      }

      function renderEmptyDurationFilterHint(panel) {
        const container = panel.querySelector("[data-active-duration-filters]");
        if (!container || container.querySelector("[data-duration-filter-card]")) return;
        container.replaceChildren();
        const hint = document.createElement("span");
        hint.className = "filter-empty-hint";
        hint.textContent = "No duration ranges active.";
        container.appendChild(hint);
      }

      function updateDurationFilterControls(panel) {
        const select = panel.querySelector("[data-duration-event-select]");
        const addButton = panel.querySelector("[data-add-duration-filter]");
        const active = activeTimelineDurationEventNames(panel);

        [...select.options].forEach(option => {
          option.disabled = option.value !== "" && active.has(option.value);
        });

        if (select.value && active.has(select.value)) {
          select.value = "";
        }
        addButton.disabled = !select.value;
      }

      function parseOptionalDuration(value) {
        const trimmed = String(value ?? "").trim();
        if (trimmed === "") return null;
        const parsed = Number(trimmed);
        return Number.isFinite(parsed) ? parsed : null;
      }

      function updateDurationRangeCard(card) {
        const minimum = parseOptionalDuration(
          card.querySelector("[data-duration-min]").value
        );
        const maximum = parseOptionalDuration(
          card.querySelector("[data-duration-max]").value
        );
        const invalid = minimum !== null && maximum !== null && minimum > maximum;
        card.classList.toggle("range-invalid", invalid);

        const summary = card.querySelector("[data-duration-summary]");
        if (invalid) {
          summary.textContent = "Minimum cannot be greater than maximum.";
        } else if (minimum === null && maximum === null) {
          summary.textContent = "No bound entered yet; this card currently keeps every span.";
        } else if (minimum !== null && maximum !== null) {
          summary.textContent = `Keeping durations from ${minimum} ms through ${maximum} ms.`;
        } else if (minimum !== null) {
          summary.textContent = `Keeping durations of at least ${minimum} ms.`;
        } else {
          summary.textContent = `Keeping durations of at most ${maximum} ms.`;
        }
      }

      function addTimelineAttributeFilter(panel, attribute) {
        const state = timelineFilterStates.get(panel);
        if (!state || !attribute || activeTimelineAttributeNames(panel).has(attribute)) return;

        const definition = state.metadata.attributes[attribute];
        if (!definition) return;

        const container = panel.querySelector("[data-active-attribute-filters]");
        if (!container.querySelector("[data-attribute-filter-card]")) {
          container.replaceChildren();
        }

        const card = document.createElement("section");
        card.className = "attribute-filter-card";
        card.dataset.attributeFilterCard = "";
        card.dataset.attributeName = attribute;

        const heading = document.createElement("div");
        heading.className = "attribute-filter-card-heading";

        const title = document.createElement("div");
        title.className = "attribute-filter-card-title";
        const name = document.createElement("strong");
        name.textContent = attribute;
        const explanation = document.createElement("span");
        explanation.textContent = "Include any checked value";
        title.append(name, explanation);

        const actions = document.createElement("div");
        actions.className = "attribute-filter-card-actions";
        [
          { action: "all", label: "All" },
          { action: "none", label: "None" },
        ].forEach(({ action, label }) => {
          const button = document.createElement("button");
          button.type = "button";
          button.dataset.attributeAction = action;
          button.textContent = label;
          button.addEventListener("click", () => {
            const checked = action === "all";
            card.querySelectorAll("[data-attribute-value]").forEach(input => {
              input.checked = checked;
            });
            applyTimelineFilters(panel);
          });
          actions.appendChild(button);
        });

        const removeButton = document.createElement("button");
        removeButton.type = "button";
        removeButton.className = "attribute-filter-remove";
        removeButton.dataset.removeAttributeFilter = "";
        removeButton.setAttribute("aria-label", `Remove ${attribute} filter`);
        removeButton.title = `Remove ${attribute} filter`;
        removeButton.textContent = "×";
        removeButton.addEventListener("click", () => {
          card.remove();
          renderEmptyAttributeFilterHint(panel);
          updateAttributeFilterControls(panel);
          applyTimelineFilters(panel);
        });
        actions.appendChild(removeButton);
        heading.append(title, actions);

        const values = document.createElement("div");
        values.className = "filter-chip-list";
        const choices = [...definition.values];
        if (definition.missingCount > 0) {
          choices.push({
            token: "__missing__",
            label: "Not present",
            count: definition.missingCount,
          });
        }

        choices.forEach((choice, index) => {
          const label = document.createElement("label");
          label.className = "filter-chip";
          const input = document.createElement("input");
          input.type = "checkbox";
          input.checked = true;
          input.value = choice.token;
          input.dataset.attributeValue = "";
          input.id = `${panel.dataset.plotId}_attribute_${state.nextAttributeFilterId}_${index}`;
          input.addEventListener("change", () => applyTimelineFilters(panel));
          const text = document.createElement("span");
          text.textContent = choice.label;
          const count = document.createElement("small");
          count.textContent = String(choice.count);
          label.append(input, text, count);
          values.appendChild(label);
        });

        state.nextAttributeFilterId += 1;
        card.append(heading, values);
        container.appendChild(card);

        const select = panel.querySelector("[data-attribute-select]");
        select.value = "";
        updateAttributeFilterControls(panel);
        applyTimelineFilters(panel);
      }

      function addTimelineDurationFilter(panel, eventName) {
        const state = timelineFilterStates.get(panel);
        if (!state || !eventName || activeTimelineDurationEventNames(panel).has(eventName)) return;

        const definition = state.metadata.events.find(event => event.name === eventName);
        if (!definition) return;

        const container = panel.querySelector("[data-active-duration-filters]");
        if (!container.querySelector("[data-duration-filter-card]")) {
          container.replaceChildren();
        }

        const card = document.createElement("section");
        card.className = "duration-filter-card";
        card.dataset.durationFilterCard = "";
        card.dataset.eventName = eventName;

        const heading = document.createElement("div");
        heading.className = "attribute-filter-card-heading";

        const title = document.createElement("div");
        title.className = "attribute-filter-card-title";
        const name = document.createElement("strong");
        name.textContent = eventName;
        const explanation = document.createElement("span");
        explanation.textContent = `Observed ${definition.minDurationMs.toFixed(3)}–${definition.maxDurationMs.toFixed(3)} ms across ${definition.count} spans`;
        title.append(name, explanation);

        const actions = document.createElement("div");
        actions.className = "attribute-filter-card-actions";
        const removeButton = document.createElement("button");
        removeButton.type = "button";
        removeButton.className = "attribute-filter-remove";
        removeButton.dataset.removeDurationFilter = "";
        removeButton.setAttribute("aria-label", `Remove ${eventName} duration filter`);
        removeButton.title = `Remove ${eventName} duration filter`;
        removeButton.textContent = "×";
        removeButton.addEventListener("click", () => {
          card.remove();
          renderEmptyDurationFilterHint(panel);
          updateDurationFilterControls(panel);
          applyTimelineFilters(panel);
        });
        actions.appendChild(removeButton);
        heading.append(title, actions);

        const fields = document.createElement("div");
        fields.className = "duration-range-fields";

        [
          { label: "Minimum ms", dataName: "durationMin", placeholder: "No minimum" },
          { label: "Maximum ms", dataName: "durationMax", placeholder: "No maximum" },
        ].forEach(definition => {
          const label = document.createElement("label");
          label.className = "duration-range-field";
          label.textContent = definition.label;
          const input = document.createElement("input");
          input.type = "number";
          input.min = "0";
          input.step = "any";
          input.placeholder = definition.placeholder;
          input.dataset[definition.dataName] = "";
          input.id = `${panel.dataset.plotId}_duration_${state.nextDurationFilterId}_${definition.dataName}`;
          input.addEventListener("input", () => {
            updateDurationRangeCard(card);
            applyTimelineFilters(panel);
          });
          label.appendChild(input);
          fields.appendChild(label);
        });

        const summary = document.createElement("div");
        summary.className = "duration-range-summary";
        summary.dataset.durationSummary = "";

        state.nextDurationFilterId += 1;
        card.append(heading, fields, summary);
        container.appendChild(card);
        updateDurationRangeCard(card);

        const select = panel.querySelector("[data-duration-event-select]");
        select.value = "";
        updateDurationFilterControls(panel);
        applyTimelineFilters(panel);
      }

      function collectTimelineAttributeFilters(panel) {
        return [...panel.querySelectorAll("[data-attribute-filter-card]")].map(card => ({
          attribute: card.dataset.attributeName,
          allowedValues: new Set(
            [...card.querySelectorAll("[data-attribute-value]:checked")]
              .map(input => input.value)
          ),
        }));
      }

      function collectTimelineDurationFilters(panel) {
        return [...panel.querySelectorAll("[data-duration-filter-card]")].map(card => {
          const minimum = parseOptionalDuration(
            card.querySelector("[data-duration-min]").value
          );
          const maximum = parseOptionalDuration(
            card.querySelector("[data-duration-max]").value
          );
          return {
            eventName: card.dataset.eventName,
            minimum,
            maximum,
            invalid: minimum !== null && maximum !== null && minimum > maximum,
          };
        });
      }

      function applyTimelineFilters(panel) {
        const state = timelineFilterStates.get(panel);
        if (!state) return;

        const enabledPrefixes = new Set(
          [...panel.querySelectorAll("[data-timeline-prefix]:checked")].map(input => input.value)
        );
        const attributeFilters = collectTimelineAttributeFilters(panel);
        const durationFilters = collectTimelineDurationFilters(panel);
        const durationFilterByEvent = new Map(
          durationFilters.map(filter => [filter.eventName, filter])
        );

        let visibleSpanCount = 0;
        let visibleTraceCount = 0;
        const visibleLanes = new Set();
        const filteredData = state.originalData.map((trace, traceIndex) => {
          const copy = cloneTimelineValue(trace);
          const prefixEnabled = enabledPrefixes.has(timelineEventPrefix(trace.name));
          const durationFilter = durationFilterByEvent.get(String(trace.name));
          const pointCount = timelineTracePointCount(trace);
          const kept = [];

          // Unknown Plotly trace storage should fail open instead of wiping the
          // complete timeline. Prefix filtering still works at trace level.
          if (pointCount === 0) {
            copy.visible = prefixEnabled;
            if (prefixEnabled) visibleTraceCount += 1;
            if (prefixEnabled && isTimelineSequence(trace.y)) {
              Array.from(trace.y).forEach(lane => visibleLanes.add(String(lane)));
            }
            return copy;
          }

          if (prefixEnabled) {
            for (let pointIndex = 0; pointIndex < pointCount; pointIndex += 1) {
              const attributes = state.pointAttributes[traceIndex][pointIndex] || {};
              const matchesAllAttributes = attributeFilters.every(filter => {
                const token = Object.prototype.hasOwnProperty.call(attributes, filter.attribute)
                  ? timelineAttributeToken(attributes[filter.attribute])
                  : "__missing__";
                return filter.allowedValues.has(token);
              });
              const durationMs = Number(trace.x[pointIndex]);
              const matchesDuration = !durationFilter || (
                !durationFilter.invalid &&
                (durationFilter.minimum === null || durationMs >= durationFilter.minimum) &&
                (durationFilter.maximum === null || durationMs <= durationFilter.maximum)
              );
              if (matchesAllAttributes && matchesDuration) {
                kept.push(pointIndex);
                if (isTimelineSequence(trace.y) && pointIndex < trace.y.length) {
                  visibleLanes.add(String(trace.y[pointIndex]));
                }
              }
            }
          }

          ["x", "y", "base", "customdata", "text", "hovertext"].forEach(key => {
            copy[key] = filterTimelineSequence(trace[key], kept, pointCount);
          });
          copy.visible = kept.length > 0;
          visibleSpanCount += kept.length;
          if (kept.length > 0) visibleTraceCount += 1;
          return copy;
        });

        const filteredLayout = cloneTimelineValue(state.originalLayout);
        const visibleLaneOrder = state.originalLaneOrder.filter(
          lane => visibleLanes.has(String(lane))
        );
        filteredLayout.yaxis = {
          ...(filteredLayout.yaxis || {}),
          categoryorder: "array",
          categoryarray: visibleLaneOrder,
        };
        filteredLayout.height = Math.max(550, 80 + 42 * visibleLaneOrder.length);

        Plotly.react(
          state.graph,
          filteredData,
          filteredLayout,
          { responsive: true, displaylogo: false }
        );

        const status = panel.querySelector("[data-filter-status]");
        const filterSummary = attributeFilters.length === 1
          ? "1 attribute filter active"
          : `${attributeFilters.length} attribute filters active`;
        const durationSummary = durationFilters.length === 1
          ? "1 duration range active"
          : `${durationFilters.length} duration ranges active`;
        status.textContent = `${visibleSpanCount} of ${state.metadata.spanCount} spans visible across ${visibleTraceCount} event types and ${visibleLaneOrder.length} lanes · ${filterSummary} · ${durationSummary}.`;
      }

      function initializeTimelineFilter(panel) {
        if (timelineFilterStates.has(panel)) return;
        const graph = document.getElementById(panel.dataset.plotId);
        const metadataElement = document.getElementById(panel.dataset.metadataId);
        if (!graph || !metadataElement || !Array.isArray(graph.data)) return;

        const metadata = JSON.parse(metadataElement.textContent);
        const originalData = cloneTimelineValue(graph.data);
        const state = {
          graph,
          metadata,
          originalData,
          originalLayout: cloneTimelineValue(graph.layout),
          originalLaneOrder: Array.isArray(graph.layout?.yaxis?.categoryarray)
            ? [...graph.layout.yaxis.categoryarray]
            : [],
          pointAttributes: originalData.map(trace => {
            if (!isTimelineSequence(trace.customdata)) return [];
            return Array.from(trace.customdata, timelinePointAttributes);
          }),
          nextAttributeFilterId: 0,
          nextDurationFilterId: 0,
        };
        timelineFilterStates.set(panel, state);

        panel.querySelectorAll("[data-timeline-prefix]").forEach(input => {
          input.addEventListener("change", () => applyTimelineFilters(panel));
        });
        panel.querySelectorAll("[data-prefix-action]").forEach(button => {
          button.addEventListener("click", () => {
            const checked = button.dataset.prefixAction === "all";
            panel.querySelectorAll("[data-timeline-prefix]").forEach(input => {
              input.checked = checked;
            });
            applyTimelineFilters(panel);
          });
        });

        const attributeSelect = panel.querySelector("[data-attribute-select]");
        const addAttributeButton = panel.querySelector("[data-add-attribute-filter]");
        attributeSelect.addEventListener("change", () => {
          updateAttributeFilterControls(panel);
        });
        addAttributeButton.addEventListener("click", () => {
          addTimelineAttributeFilter(panel, attributeSelect.value);
        });

        const durationEventSelect = panel.querySelector("[data-duration-event-select]");
        const addDurationButton = panel.querySelector("[data-add-duration-filter]");
        durationEventSelect.addEventListener("change", () => {
          updateDurationFilterControls(panel);
        });
        addDurationButton.addEventListener("click", () => {
          addTimelineDurationFilter(panel, durationEventSelect.value);
        });

        panel.querySelector("[data-filter-reset]").addEventListener("click", () => {
          panel.querySelectorAll("[data-timeline-prefix]").forEach(input => {
            input.checked = true;
          });
          panel.querySelectorAll("[data-attribute-filter-card]").forEach(card => card.remove());
          panel.querySelectorAll("[data-duration-filter-card]").forEach(card => card.remove());
          attributeSelect.value = "";
          durationEventSelect.value = "";
          renderEmptyAttributeFilterHint(panel);
          renderEmptyDurationFilterHint(panel);
          updateAttributeFilterControls(panel);
          updateDurationFilterControls(panel);
          applyTimelineFilters(panel);
        });

        renderEmptyAttributeFilterHint(panel);
        renderEmptyDurationFilterHint(panel);
        updateAttributeFilterControls(panel);
        updateDurationFilterControls(panel);
        applyTimelineFilters(panel);
      }

      function updateOverviewEventRows(card) {
        const expanded = card.dataset.overviewExpanded === "true";
        const search = (card.querySelector("[data-overview-search]")?.value || "").trim().toLowerCase();
        const rows = Array.from(card.querySelectorAll("[data-overview-event-row]"));
        let visible = 0;
        rows.forEach(row => {
          const matchesSearch = !search || (row.dataset.eventName || "").toLowerCase().includes(search);
          const matchesLimit = expanded || Number(row.dataset.rank) <= 10 || Boolean(search);
          row.hidden = !(matchesSearch && matchesLimit);
          if (!row.hidden) visible += 1;
        });
        const status = card.querySelector("[data-overview-status]");
        if (status) {
          status.textContent = search
            ? `${visible} matching event types of ${rows.length}`
            : expanded
              ? `Showing all ${rows.length} event types`
              : `Showing top ${Math.min(10, rows.length)} of ${rows.length}`;
        }
        const toggle = card.querySelector("[data-overview-toggle]");
        if (toggle) toggle.textContent = expanded ? "Show top 10" : `Show all ${rows.length}`;
      }

      function initializeOverviewCard(card) {
        if (card.dataset.overviewInitialized === "true") return;
        card.dataset.overviewInitialized = "true";
        card.querySelector("[data-overview-search]")?.addEventListener("input", () => updateOverviewEventRows(card));
        card.querySelector("[data-overview-toggle]")?.addEventListener("click", () => {
          card.dataset.overviewExpanded = card.dataset.overviewExpanded === "true" ? "false" : "true";
          updateOverviewEventRows(card);
        });
        updateOverviewEventRows(card);
      }

      const comparisonState = { initialized: false, metadata: null };

      function comparisonFamilyLabel(name) {
        const labels = {
          full_bfs: "Total BFS",
          bfs: "BFS",
          neo4j: "Neo4j",
          arango: "ArangoDB",
          cugraph: "GPU / cuGraph",
          gpu: "GPU",
          redis: "Redis",
          mmap: "MMAP",
          postgresql: "PostgreSQL",
          backend: "Backend scheduler",
        };
        return labels[name] || name;
      }

      function checkedComparisonValues(selector) {
        return new Set(
          Array.from(document.querySelectorAll(selector))
            .filter(input => input.checked)
            .map(input => input.value)
        );
      }

      function comparisonMetricDefinition(key) {
        if (!comparisonState.metadata) return null;
        return comparisonState.metadata.metrics.find(metric => metric.key === key) || null;
      }

      function comparisonMetricValue(run, key) {
        const separator = key.indexOf(":");
        if (separator < 0) return null;
        const kind = key.slice(0, separator);
        const name = key.slice(separator + 1);
        const container = kind === "family" ? run.groups : run.events;
        if (!container || !Object.prototype.hasOwnProperty.call(container, name)) return null;
        const value = Number(container[name]);
        return Number.isFinite(value) ? value : null;
      }

      function comparisonQuantile(sortedValues, probability) {
        if (!sortedValues.length) return null;
        if (sortedValues.length === 1) return sortedValues[0];
        const position = (sortedValues.length - 1) * probability;
        const lower = Math.floor(position);
        const upper = Math.ceil(position);
        if (lower === upper) return sortedValues[lower];
        const fraction = position - lower;
        return sortedValues[lower] + (sortedValues[upper] - sortedValues[lower]) * fraction;
      }

      function comparisonStats(values) {
        const sorted = values.filter(Number.isFinite).slice().sort((a, b) => a - b);
        if (!sorted.length) return null;
        return {
          n: sorted.length,
          min: sorted[0],
          q1: comparisonQuantile(sorted, 0.25),
          median: comparisonQuantile(sorted, 0.5),
          q3: comparisonQuantile(sorted, 0.75),
          max: sorted[sorted.length - 1],
        };
      }

      function comparisonFormatMs(value) {
        if (!Number.isFinite(value)) return "—";
        if (Math.abs(value) >= 1000) return value.toLocaleString(undefined, { maximumFractionDigits: 1 }) + " ms";
        return value.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 3 }) + " ms";
      }

      function comparisonSelectedSamples() {
        const metadata = comparisonState.metadata;
        const groups = checkedComparisonValues("[data-comparison-group]");
        const sources = checkedComparisonValues("[data-comparison-source]");
        return metadata.runs.filter(run => groups.has(run.group) && sources.has(run.sourceKind));
      }

      function comparisonStatsForGroup(samples, group, metricKey) {
        return comparisonStats(
          samples
            .filter(run => run.group === group)
            .map(run => comparisonMetricValue(run, metricKey))
            .filter(Number.isFinite)
        );
      }

      function comparisonOrderedGroups(samples, metricKey) {
        const selected = checkedComparisonValues("[data-comparison-group]");
        const definitions = comparisonState.metadata.runGroups.filter(group => selected.has(group.name));
        const sortMode = document.querySelector("[data-comparison-sort]")?.value || "name";
        if (sortMode !== "median") return definitions.map(group => group.name);
        return definitions
          .map((group, index) => ({
            name: group.name,
            index,
            stats: comparisonStatsForGroup(samples, group.name, metricKey),
          }))
          .sort((left, right) => {
            const a = left.stats?.median;
            const b = right.stats?.median;
            if (!Number.isFinite(a) && !Number.isFinite(b)) return left.index - right.index;
            if (!Number.isFinite(a)) return 1;
            if (!Number.isFinite(b)) return -1;
            return a - b || left.index - right.index;
          })
          .map(group => group.name);
      }

      function comparisonPlotLayout(yTitle, groupCount, traceCount = 0) {
        const logarithmic = document.querySelector("[data-comparison-scale]")?.value === "log";
        const manyTraces = traceCount > 4;
        return {
          paper_bgcolor: "white",
          plot_bgcolor: "white",
          font: { color: "#111827", size: 12 },
          margin: {
            l: 78,
            r: manyTraces ? 185 : 28,
            t: manyTraces ? 30 : (traceCount ? 78 : 28),
            b: groupCount > 8 ? 105 : 72,
          },
          xaxis: {
            title: "Run configuration",
            tickangle: groupCount > 8 ? -24 : 0,
            automargin: true,
            tickfont: { size: 11 },
            categoryorder: "array",
          },
          yaxis: {
            title: yTitle,
            type: logarithmic ? "log" : "linear",
            rangemode: logarithmic ? undefined : "tozero",
            automargin: true,
            tickformat: "~s",
            gridcolor: "#e5e7eb",
            zerolinecolor: "#cbd5e1",
          },
          legend: manyTraces
            ? { orientation: "v", x: 1.01, xanchor: "left", y: 1, yanchor: "top", font: { size: 11 } }
            : { orientation: "h", x: 0, y: 1.13, xanchor: "left", yanchor: "bottom", font: { size: 11 } },
          hovermode: "closest",
          bargap: 0.16,
          bargroupgap: 0.06,
        };
      }

      function comparisonEmptyPlot(id, message) {
        const graph = document.getElementById(id);
        if (!graph) return;
        Plotly.react(graph, [], {
          paper_bgcolor: "white",
          plot_bgcolor: "white",
          xaxis: { visible: false },
          yaxis: { visible: false },
          annotations: [{
            text: message,
            showarrow: false,
            font: { color: "#64748b", size: 14 },
          }],
          margin: { l: 30, r: 30, t: 30, b: 30 },
        }, { responsive: true, displaylogo: false });
      }

      function renderComparisonSummary(groupOrder, samples, metricKey) {
        const body = document.querySelector("[data-comparison-summary-body]");
        if (!body) return;
        body.replaceChildren();
        const rows = groupOrder
          .map(group => ({ group, stats: comparisonStatsForGroup(samples, group, metricKey) }))
          .filter(row => row.stats);
        if (!rows.length) {
          const row = document.createElement("tr");
          const cell = document.createElement("td");
          cell.colSpan = 7;
          cell.className = "comparison-no-data";
          cell.textContent = "No samples contain the selected metric.";
          row.appendChild(cell);
          body.appendChild(row);
          return;
        }
        const best = Math.min(...rows.map(row => row.stats.median));
        rows.forEach(({ group, stats }) => {
          const row = document.createElement("tr");
          row.dataset.best = stats.median === best ? "true" : "false";
          const delta = best > 0 ? ((stats.median / best) - 1) * 100 : 0;
          const cells = [
            group,
            String(stats.n),
            comparisonFormatMs(stats.median),
            comparisonFormatMs(stats.q3 - stats.q1),
            comparisonFormatMs(stats.min),
            comparisonFormatMs(stats.max),
            stats.median === best ? "best" : `+${delta.toFixed(1)}%`,
          ];
          cells.forEach(value => {
            const cell = document.createElement("td");
            cell.textContent = value;
            row.appendChild(cell);
          });
          body.appendChild(row);
        });
      }

      function comparisonAggregateTrace(name, label, groupOrder, samples, kind) {
        const medians = [];
        const plus = [];
        const minus = [];
        const customdata = [];
        groupOrder.forEach(group => {
          const values = samples
            .filter(run => run.group === group)
            .map(run => {
              const container = kind === "family" ? run.groups : run.events;
              if (!container || !Object.prototype.hasOwnProperty.call(container, name)) return null;
              const value = Number(container[name]);
              return Number.isFinite(value) ? value : null;
            })
            .filter(Number.isFinite);
          const stats = comparisonStats(values);
          medians.push(stats ? stats.median : null);
          plus.push(stats ? stats.q3 - stats.median : 0);
          minus.push(stats ? stats.median - stats.q1 : 0);
          customdata.push(stats ? [stats.n, stats.q1, stats.q3] : [0, null, null]);
        });
        return {
          type: "bar",
          name: label,
          x: groupOrder,
          y: medians,
          customdata,
          error_y: {
            type: "data",
            symmetric: false,
            array: plus,
            arrayminus: minus,
            visible: true,
            thickness: 1.2,
            width: 3,
          },
          hovertemplate:
            "<b>%{x}</b><br>" + label +
            ": %{y:.3f} ms<br>IQR: %{customdata[1]:.3f}–%{customdata[2]:.3f} ms" +
            "<br>n=%{customdata[0]}<extra></extra>",
        };
      }

      function renderComparison() {
        if (!comparisonState.metadata) return;
        const metadata = comparisonState.metadata;
        const enabledCharts = checkedComparisonValues("[data-comparison-chart-toggle]");
        const selectedFamilies = checkedComparisonValues("[data-comparison-family]");
        const selectedEvents = checkedComparisonValues("[data-comparison-event]");
        const metricSelect = document.querySelector("[data-comparison-primary-metric]");
        const metricKey = metricSelect?.value || metadata.defaultMetric;
        const metric = comparisonMetricDefinition(metricKey);
        const samples = comparisonSelectedSamples();
        const groupOrder = comparisonOrderedGroups(samples, metricKey);

        document.querySelectorAll("[data-comparison-chart]").forEach(card => {
          card.hidden = !enabledCharts.has(card.dataset.comparisonChart);
        });
        document.querySelectorAll("[data-comparison-primary-label]").forEach(node => {
          node.textContent = metric?.label || "Selected metric";
        });

        const status = document.querySelector("[data-comparison-status]");
        const serverEnabled = checkedComparisonValues("[data-comparison-source]").has("server");
        if (status) {
          status.textContent = `${groupOrder.length} run configurations · ${samples.length} samples · ${metric?.label || "no metric"}` +
            (serverEnabled ? " · server.events.jsonl included" : " · server.events.jsonl excluded");
        }

        if (!groupOrder.length || !samples.length || !metricKey) {
          comparisonEmptyPlot("comparison_boxplot", "Select at least one run configuration and sample source.");
          comparisonEmptyPlot("comparison_families", "Select at least one run configuration and sample source.");
          comparisonEmptyPlot("comparison_events", "Select at least one run configuration and sample source.");
          renderComparisonSummary([], [], metricKey);
          return;
        }

        const boxTraces = groupOrder.map(group => {
          const groupSamples = samples.filter(run => run.group === group);
          const values = [];
          const labels = [];
          groupSamples.forEach(run => {
            const value = comparisonMetricValue(run, metricKey);
            if (Number.isFinite(value)) {
              values.push(value);
              labels.push(run.label);
            }
          });
          return {
            type: "box",
            name: group,
            y: values,
            text: labels,
            quartilemethod: "linear",
            boxpoints: values.length <= 40 ? "all" : "outliers",
            jitter: 0.24,
            pointpos: 0,
            marker: { size: 5, opacity: 0.55 },
            line: { width: 1.5 },
            hovertemplate:
              `<b>${group}</b><br>${metric?.label || "metric"}: %{y:.3f} ms` +
              "<br>%{text}<extra></extra>",
          };
        }).filter(trace => trace.y.length);

        if (!boxTraces.length) {
          comparisonEmptyPlot("comparison_boxplot", "The selected samples do not contain this metric.");
        } else {
          const layout = comparisonPlotLayout("Execution time [ms]", groupOrder.length, 0);
          layout.showlegend = false;
          layout.xaxis.categoryarray = groupOrder;
          Plotly.react("comparison_boxplot", boxTraces, layout, { responsive: true, displaylogo: false });
        }
        renderComparisonSummary(groupOrder, samples, metricKey);

        const familyOrder = metadata.families
          .map(family => family.name)
          .filter(name => selectedFamilies.has(name));
        if (!familyOrder.length) {
          comparisonEmptyPlot("comparison_families", "Select backend/event families at left.");
        } else {
          const familyTraces = familyOrder.map(family =>
            comparisonAggregateTrace(family, comparisonFamilyLabel(family), groupOrder, samples, "family")
          );
          const layout = comparisonPlotLayout("Median active wall time [ms]", groupOrder.length, familyTraces.length);
          layout.barmode = "group";
          layout.xaxis.categoryarray = groupOrder;
          Plotly.react("comparison_families", familyTraces, layout, { responsive: true, displaylogo: false });
        }

        const eventOrder = metadata.events
          .map(event => event.name)
          .filter(name => selectedEvents.has(name));
        if (!eventOrder.length) {
          comparisonEmptyPlot("comparison_events", "Select individual events at left.");
        } else {
          const eventTraces = eventOrder.map(eventName =>
            comparisonAggregateTrace(eventName, eventName, groupOrder, samples, "event")
          );
          const layout = comparisonPlotLayout("Median active wall time [ms]", groupOrder.length, eventTraces.length);
          layout.barmode = "group";
          layout.xaxis.categoryarray = groupOrder;
          Plotly.react("comparison_events", eventTraces, layout, { responsive: true, displaylogo: false });
        }
      }

      function setComparisonChecks(selector, predicate) {
        document.querySelectorAll(selector).forEach((input, index) => {
          input.checked = Boolean(predicate(input, index));
        });
        renderComparison();
      }

      function initializeComparison() {
        if (comparisonState.initialized) return;
        const metadataElement = document.getElementById("comparison_metadata");
        if (!metadataElement) return;
        comparisonState.initialized = true;
        comparisonState.metadata = JSON.parse(metadataElement.textContent);

        document.querySelectorAll(
          "[data-comparison-group], [data-comparison-source], [data-comparison-family], " +
          "[data-comparison-event], [data-comparison-chart-toggle], [data-comparison-primary-metric], " +
          "[data-comparison-sort], [data-comparison-scale]"
        ).forEach(input => input.addEventListener("change", renderComparison));

        document.querySelectorAll("[data-comparison-groups]").forEach(button => {
          button.addEventListener("click", () => {
            setComparisonChecks("[data-comparison-group]", () => button.dataset.comparisonGroups === "all");
          });
        });
        document.querySelectorAll("[data-comparison-families]").forEach(button => {
          button.addEventListener("click", () => {
            const action = button.dataset.comparisonFamilies;
            const runtime = new Set(["neo4j", "arango", "cugraph", "gpu", "redis", "mmap", "postgresql", "backend"]);
            setComparisonChecks(
              "[data-comparison-family]",
              input => action === "all" || (action === "runtime" && runtime.has(input.value))
            );
          });
        });
        document.querySelectorAll("[data-comparison-events]").forEach(button => {
          button.addEventListener("click", () => {
            const action = button.dataset.comparisonEvents;
            setComparisonChecks(
              "[data-comparison-event]",
              (_input, index) => action === "all" || (action === "top" && index < 6)
            );
          });
        });
        document.querySelector("[data-comparison-event-search]")?.addEventListener("input", event => {
          const query = event.target.value.trim().toLowerCase();
          document.querySelectorAll("[data-comparison-event-row]").forEach(row => {
            row.hidden = Boolean(query) && !(row.dataset.eventSearch || "").includes(query);
          });
        });
        renderComparison();
      }

      function updateDashboard() {
        const runSelect = document.getElementById("runSelect");
        const run = runSelect.value;
        const view = document.getElementById("viewSelect").value;
        const comparing = view === "comparison";

        document.querySelectorAll(".chart-panel").forEach(panel => {
          const active = comparing
            ? panel.dataset.view === "comparison"
            : panel.dataset.run === run && panel.dataset.view === view;
          panel.classList.toggle("active", active);
        });

        const runControl = document.getElementById("runControl");
        if (runControl) runControl.hidden = comparing;
        runSelect.disabled = comparing;
        if (comparing) renderComparison();

        setTimeout(() => {
          window.dispatchEvent(new Event("resize"));
        }, 50);
      }

      document.addEventListener("DOMContentLoaded", () => {
        document.querySelectorAll("[data-timeline-filter]").forEach(initializeTimelineFilter);
        document.querySelectorAll("[data-overview-card]").forEach(initializeOverviewCard);
        initializeComparison();
        document.getElementById("runSelect").addEventListener("change", updateDashboard);
        document.getElementById("viewSelect").addEventListener("change", updateDashboard);
        updateDashboard();
      });
    </script>
    """

    document = f"""
    <!doctype html>
    <html>
    <head>
      <meta charset="utf-8" />
      <title>C++ Event Log Dashboard</title>
      {plotly_script}
      {css}
    </head>

    <body>
      <header>
        <h1>C++ Event Log Dashboard</h1>

        <div class="controls">
          <label id="runControl">
            Run
            <select id="runSelect">
              {run_options}
            </select>
          </label>

          <label>
            View
            <select id="viewSelect">
              {view_options}
            </select>
          </label>

          {average_index_link}
        </div>

        <div class="hint">
          Pass a measurement directory or its logs directory. Repeated logs are averaged per UUID directory and globally, then shown as selectable Average executions.
        </div>
      </header>

      <main>
        {panels_html}
      </main>

      {js}
    </body>
    </html>
    """

    output_html.write_text(document, encoding="utf-8")
    print(f"Wrote dashboard: {output_html}")


# ============================================================
# Main
# ============================================================

def main():
    # Extended research/Prometheus modes live in a small orchestration module so
    # the already-large reconstruction/dashboard implementation remains focused.
    extended_flags = {
        "--metrics-config",
        "--refresh-metrics",
        "--split-by-run-type",
        "--compare-only",
        "--with-compare",
        "--include-server-in-compare",
    }
    if any(argument.split("=", 1)[0] in extended_flags for argument in sys.argv[1:]):
        from tools.measurement.log_reconstruction_plus import main as extended_main
        return extended_main(sys.argv[1:])

    parser = argparse.ArgumentParser(
        description="Generate a dashboard for C++ JSONL event logs."
    )

    parser.add_argument(
        "input",
        type=Path,
        help="A JSONL file or a directory containing JSONL logs."
    )

    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("log_viz"),
        help=(
            "Single output directory for the dashboard, reconstructed span CSVs, "
            "per-directory averages, and global average. Source logs remain unchanged."
        )
    )

    parser.add_argument(
        "--pattern",
        default="*.jsonl",
        help="Glob pattern when input is a directory. Default: *.jsonl"
    )

    parser.add_argument(
        "--inline-plotly",
        action="store_true",
        help="Embed Plotly JS into the HTML for offline viewing. Makes the file larger."
    )

    parser.add_argument(
        "--no-averages",
        action="store_true",
        help="Do not regenerate or include per-directory/global Average executions."
    )

    args = parser.parse_args()

    log_root = resolve_log_root(args.input)
    args.out_dir = args.out_dir.expanduser().resolve()
    if log_root.is_dir() and (args.out_dir == log_root or args.out_dir.is_relative_to(log_root)):
        raise SystemExit(
            "--out-dir must be outside the source logs directory so reconstruction "
            "never modifies the original logs."
        )
    args.out_dir.mkdir(parents=True, exist_ok=True)

    average_analysis = None
    if log_root.is_dir() and not args.no_averages:
        average_analysis = analyze_log_tree(
            log_root,
            output_root=args.out_dir,
        )
        print(f"Generated average reports inside: {args.out_dir}")

    log_files = discover_log_files(log_root, pattern=args.pattern)
    average_entries = average_execution_entries(average_analysis)

    if not log_files and not average_entries:
        raise SystemExit(f"No log files found under {log_root}")

    root = log_root if log_root.is_dir() else log_root.parent
    raw_runs = []
    average_runs = []
    errors = []

    def load_run(
            log_file: Path,
            *,
            is_average: bool,
            label_override: str | None = None,
    ) -> None:
        try:
            events = load_jsonl(log_file)
            spans = pair_spans(events)

            if is_average:
                run_id = "average__" + make_run_id(log_file.parent, args.out_dir)
                label = label_override or f"Average — {log_file.parent.name}"
            else:
                run_id = make_run_id(log_file, root)
                rel = log_file.relative_to(root) if log_file.is_relative_to(root) else log_file
                label = str(rel)

            safe_id = make_safe_html_id(run_id)
            rel = log_file.relative_to(root) if log_file.is_relative_to(root) else log_file
            spans_csv = args.out_dir / "spans" / f"{safe_id}.spans.csv"
            spans_csv.parent.mkdir(parents=True, exist_ok=True)
            spans.drop(
                columns=["start_event", "end_event", "attributes"],
                errors="ignore",
            ).to_csv(spans_csv, index=False)
            run = {
                "id": run_id,
                "safe_id": safe_id,
                "label": label,
                "relative_path": rel,
                "log_file": log_file,
                "events": events,
                "spans": spans,
                "spans_csv": spans_csv,
                "is_average": is_average,
            }
            (average_runs if is_average else raw_runs).append(run)
            kind = "average" if is_average else "run"
            print(f"Loaded {kind} {log_file} -> {len(events)} events, {len(spans)} spans")

        except Exception as e:
            errors.append((log_file, str(e)))
            print(f"Skipping {log_file}: {e}")

    for log_file in log_files:
        load_run(log_file, is_average=False)
    for log_file, label in average_entries:
        load_run(log_file, is_average=True, label_override=label)

    runs = average_runs + raw_runs
    if not runs:
        print("No valid logs found.")
        if errors:
            print("Errors:")
            for path, err in errors:
                print(f"  {path}: {err}")
        raise SystemExit(1)

    dashboard_path = args.out_dir / "dashboard.html"
    average_index = (
        Path(average_analysis["index"])
        if average_analysis and average_analysis.get("index")
        else None
    )

    build_dashboard(
        runs=runs,
        output_html=dashboard_path,
        inline_plotly=args.inline_plotly,
        average_index=average_index,
    )

    print()
    print(f"All reconstruction artifacts: {args.out_dir.resolve()}")
    print("Open:")
    print(f"  xdg-open {dashboard_path}")


if __name__ == "__main__":
    main()