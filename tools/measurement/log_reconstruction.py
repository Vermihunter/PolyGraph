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

from tools.event_log_average import AVERAGE_DIR_NAME, analyze_log_tree


STRUCTURAL_EVENT_FIELDS = {
    "event",
    "seq",
    "steady_ns",
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


def load_average_artifacts(execution_file: Path) -> tuple[dict, pd.DataFrame, Path | None]:
    average_dir = execution_file.parent
    summary_path = average_dir / "summary.json"
    events_path = average_dir / "events.csv"
    report_path = average_dir / "report.html"

    summary = {}
    if summary_path.is_file():
        summary = json.loads(summary_path.read_text(encoding="utf-8"))

    events = pd.DataFrame()
    if events_path.is_file() and events_path.stat().st_size > 0:
        events = pd.read_csv(events_path)

    return summary, events, report_path if report_path.is_file() else None


# ============================================================
# Loading
# ============================================================

def load_jsonl(path: Path) -> pd.DataFrame:
    rows = []

    with path.open("r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, start=1):
            line = line.strip()

            if not line:
                continue

            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                raise ValueError(f"Invalid JSON on line {line_no}: {e}") from e

    if not rows:
        raise ValueError(f"No events found in {path}")

    df = pd.DataFrame(rows)

    required = {"event", "seq", "steady_ns", "thread_id"}
    missing = required - set(df.columns)

    if missing:
        raise ValueError(f"Missing required columns: {missing}")

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


def build_attributes_html(run: dict) -> str:
    """Render every dynamic span attribute as a searchable long-form table."""
    spans = run["spans"]
    rows: list[str] = []
    attribute_names: set[str] = set()

    for _, span in spans.iterrows():
        attributes = span.get("attributes")
        if not isinstance(attributes, dict):
            continue
        for key, value in sorted(attributes.items(), key=lambda item: _attribute_sort_key(item[0])):
            attribute_names.add(key)
            rows.append(
                "<tr>"
                f"<td>{html.escape(str(span['name']))}</td>"
                f"<td>{float(span['start_ms']):.3f}</td>"
                f"<td>{float(span['duration_ms']):.3f}</td>"
                f"<td>{int(span.get('thread_index', span['thread_id']))}</td>"
                f"<td>{html.escape(str(key))}</td>"
                f"<td><code>{html.escape(format_attribute_value(value))}</code></td>"
                "</tr>"
            )

    if not rows:
        return unavailable_view("This run has no custom event attributes.")

    table_id = f"attributes_{run['safe_id']}"
    return f"""
    <div class="attributes-view">
      <div class="attributes-heading">
        <div>
          <h2>Span attributes</h2>
          <p>{len(attribute_names)} distinct fields across {len(rows)} attribute values.</p>
        </div>
        <input
          class="attribute-filter"
          type="search"
          placeholder="Filter event, attribute, or value…"
          oninput="filterAttributeTable('{table_id}', this.value)"
        />
      </div>
      <div class="attribute-table-wrap">
        <table class="attribute-table" id="{table_id}">
          <thead><tr>
            <th>Event</th><th>Start ms</th><th>Duration ms</th>
            <th>Thread</th><th>Attribute</th><th>Value</th>
          </tr></thead>
          <tbody>{''.join(rows)}</tbody>
        </table>
      </div>
    </div>
    """


def display_thread_index(row) -> int:
    value = row.get("thread_index", row["thread_id"])
    return int(value)


def make_lane_by_thread(row) -> str:
    return f"thread {display_thread_index(row)} / lane {int(row['overlap_lane'])}"


def make_lane_by_thread_and_event(row) -> str:
    return (
        f"thread {display_thread_index(row)} / {row['name']} "
        f"/ lane {int(row['overlap_lane'])}"
    )


def make_lane_by_depth(row) -> str:
    if pd.notna(row.get("depth")):
        return f"depth {int(row['depth'])} / lane {int(row['overlap_lane'])}"

    return f"no depth / lane {int(row['overlap_lane'])}"


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


def build_event_summary_figure(spans: pd.DataFrame) -> go.Figure:
    summary = (
        spans
        .groupby("name", as_index=False)
        .agg(
            total_ms=("duration_ms", "sum"),
            mean_ms=("duration_ms", "mean"),
            max_ms=("duration_ms", "max"),
            count=("duration_ms", "count"),
        )
        .sort_values("total_ms", ascending=True)
    )

    fig = go.Figure()

    fig.add_trace(go.Bar(
        x=summary["total_ms"],
        y=summary["name"],
        orientation="h",
        customdata=summary[["mean_ms", "max_ms", "count"]],
        hovertemplate=(
            "<b>%{y}</b><br>"
            "total: %{x:.3f} ms<br>"
            "mean: %{customdata[0]:.3f} ms<br>"
            "max: %{customdata[1]:.3f} ms<br>"
            "count: %{customdata[2]}"
            "<extra></extra>"
        ),
    ))

    fig.update_layout(
        title="Total Time by Event Type",
        xaxis_title="Total time [ms]",
        yaxis_title="Event type",
        height=max(450, 120 + 45 * len(summary)),
        margin={
            "l": 230,
            "r": 40,
            "t": 80,
            "b": 60,
        },
    )

    return fig


def build_depth_summary_figure(spans: pd.DataFrame) -> go.Figure | None:
    df = spans[pd.notna(spans["depth"])].copy()

    if df.empty:
        return None

    df["depth"] = df["depth"].astype(int)

    summary = (
        df
        .groupby(["depth", "name"], as_index=False)
        .agg(total_ms=("duration_ms", "sum"))
        .sort_values(["depth", "name"])
    )

    fig = go.Figure()

    for event_name, group in summary.groupby("name", sort=False):
        fig.add_trace(go.Bar(
            x=group["depth"],
            y=group["total_ms"],
            name=event_name,
            hovertemplate=(
                "<b>%{fullData.name}</b><br>"
                "depth: %{x}<br>"
                "total: %{y:.3f} ms"
                "<extra></extra>"
            ),
        ))

    fig.update_layout(
        title="Time by BFS Depth",
        xaxis_title="BFS depth",
        yaxis_title="Total time [ms]",
        barmode="stack",
        height=550,
    )

    return fig


def build_size_vs_duration_figure(spans: pd.DataFrame) -> go.Figure | None:
    rows = []

    for _, row in spans.iterrows():
        if pd.notna(row.get("row_size")):
            rows.append({
                "name": row["name"],
                "size_kind": "row_size",
                "size": row["row_size"],
                "duration_ms": row["duration_ms"],
                "depth": row.get("depth"),
            })

        if pd.notna(row.get("frontier_size")):
            rows.append({
                "name": row["name"],
                "size_kind": "frontier_size",
                "size": row["frontier_size"],
                "duration_ms": row["duration_ms"],
                "depth": row.get("depth"),
            })

    if not rows:
        return None

    df = pd.DataFrame(rows)

    fig = go.Figure()

    for label, group in df.groupby(["name", "size_kind"], sort=False):
        event_name, size_kind = label

        fig.add_trace(go.Scatter(
            x=group["size"],
            y=group["duration_ms"],
            mode="markers",
            name=f"{event_name} / {size_kind}",
            customdata=group[["depth"]],
            hovertemplate=(
                "<b>%{fullData.name}</b><br>"
                "size: %{x}<br>"
                "duration: %{y:.3f} ms<br>"
                "depth: %{customdata[0]}"
                "<extra></extra>"
            ),
        ))

    fig.update_layout(
        title="Input Size vs Duration",
        xaxis_title="row_size / frontier_size",
        yaxis_title="Duration [ms]",
        height=600,
    )

    return fig


def build_average_event_figure(events: pd.DataFrame, title: str) -> go.Figure | None:
    required = {
        "event",
        "avg_total_ms_per_run",
        "p95_total_ms_per_run",
        "stddev_total_ms_per_run",
    }
    if events.empty or not required.issubset(events.columns):
        return None

    top = (
        events
        .sort_values("avg_total_ms_per_run", ascending=False)
        .head(30)
        .sort_values("avg_total_ms_per_run", ascending=True)
    )

    fig = go.Figure()
    fig.add_trace(go.Bar(
        x=top["avg_total_ms_per_run"],
        y=top["event"],
        orientation="h",
        name="Average total / execution",
        error_x={
            "type": "data",
            "array": top["stddev_total_ms_per_run"],
            "visible": True,
        },
        customdata=top[[
            "avg_count_per_run",
            "avg_span_ms",
            "presence_pct",
            "inclusive_share_pct",
        ]],
        hovertemplate=(
            "<b>%{y}</b><br>"
            "average total: %{x:.4f} ms<br>"
            "average count: %{customdata[0]:.2f}<br>"
            "average span: %{customdata[1]:.4f} ms<br>"
            "presence: %{customdata[2]:.1f}%<br>"
            "inclusive share: %{customdata[3]:.2f}%"
            "<extra></extra>"
        ),
    ))
    fig.add_trace(go.Scatter(
        x=top["p95_total_ms_per_run"],
        y=top["event"],
        mode="markers",
        name="P95 total / execution",
        marker={"size": 9, "symbol": "diamond"},
        hovertemplate=(
            "<b>%{y}</b><br>"
            "P95 total: %{x:.4f} ms"
            "<extra></extra>"
        ),
    ))
    fig.update_layout(
        title=title,
        xaxis_title="Inclusive time per execution [ms]",
        yaxis_title="Event type",
        height=max(620, 150 + 32 * len(top)),
        margin={"l": 300, "r": 50, "t": 85, "b": 65},
        legend={"orientation": "h", "y": 1.07, "x": 0},
    )
    return fig


def build_average_profile_html(run: dict) -> str:
    if not run.get("is_average"):
        return unavailable_view(
            "This view is available for generated Average executions. "
            "Choose an Average entry in the Run selector."
        )

    summary = run.get("average_summary") or {}
    events = run.get("average_events")
    if not isinstance(events, pd.DataFrame):
        events = pd.DataFrame()

    def metric(name: str, default=0):
        value = summary.get(name, default)
        return value if value is not None else default

    report = run.get("average_report")
    report_href = run.get("average_report_href")
    report_link = (
        f'<a class="average-report-link" href="{html.escape(str(report_href))}">'
        "Open the standalone average report</a>"
        if isinstance(report, Path) and report.is_file() and report_href
        else ""
    )

    cards = f"""
    <div class="average-cards">
      <div class="average-card"><span>Executions</span><strong>{int(metric('validRuns'))}</strong></div>
      <div class="average-card"><span>Average window</span><strong>{float(metric('averageWindowMs')):.3f} ms</strong></div>
      <div class="average-card"><span>P95 window</span><strong>{float(metric('p95WindowMs')):.3f} ms</strong></div>
      <div class="average-card"><span>Std. deviation</span><strong>{float(metric('stddevWindowMs')):.3f} ms</strong></div>
      <div class="average-card"><span>Event types</span><strong>{int(metric('eventTypeCount'))}</strong></div>
      <div class="average-card"><span>Source spans</span><strong>{int(metric('spanCount')):,}</strong></div>
    </div>
    """

    chart = build_average_event_figure(
        events,
        title=f"Average and P95 Event Time — {run['label']}",
    )
    chart_html = (
        figure_to_div(chart, div_id=f"{run['safe_id']}_average_event_chart")
        if chart is not None
        else unavailable_view("The average event table is missing or empty.")
    )

    rows = []
    if not events.empty:
        ordered = events.sort_values("avg_total_ms_per_run", ascending=False)
        for _, row in ordered.iterrows():
            rows.append(f"""
            <tr>
              <td>{html.escape(str(row['event']))}</td>
              <td>{float(row['avg_total_ms_per_run']):.4f}</td>
              <td>{float(row['p95_total_ms_per_run']):.4f}</td>
              <td>{float(row['stddev_total_ms_per_run']):.4f}</td>
              <td>{float(row['avg_count_per_run']):.2f}</td>
              <td>{float(row['avg_span_ms']):.4f}</td>
              <td>{float(row['presence_pct']):.1f}%</td>
              <td>{float(row['inclusive_share_pct']):.2f}%</td>
            </tr>
            """)

    table_html = (
        f"""
        <div class="average-table-wrap">
          <table class="average-table">
            <thead><tr>
              <th>Event</th><th>Avg total/run ms</th><th>P95 ms</th>
              <th>Stddev ms</th><th>Avg count/run</th><th>Avg span ms</th>
              <th>Presence</th><th>Inclusive share</th>
            </tr></thead>
            <tbody>{''.join(rows)}</tbody>
          </table>
        </div>
        """
        if rows
        else unavailable_view("No average event rows were generated.")
    )

    return f"""
    <div class="average-profile">
      <div class="average-profile-heading">
        <div><h2>Average execution profile</h2><p>Inclusive event timing averaged across repeated executions.</p></div>
        {report_link}
      </div>
      {cards}
      <div class="average-chart">{chart_html}</div>
      <h3>Complete event timing table</h3>
      {table_html}
    </div>
    """


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

    top = (
        spans
        .groupby("name", as_index=False)
        .agg(total_ms=("duration_ms", "sum"), count=("duration_ms", "count"))
        .sort_values("total_ms", ascending=False)
        .head(10)
    )

    rows = "\n".join(
        f"""
        <tr>
          <td>{html.escape(str(r["name"]))}</td>
          <td>{r["total_ms"]:.3f}</td>
          <td>{int(r["count"])}</td>
        </tr>
        """
        for _, r in top.iterrows()
    )

    return f"""
    <div class="overview-card">
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

      <h3>Top event types by total time</h3>

      <table class="meta-table">
        <tr>
          <th>Event type</th>
          <th>Total ms</th>
          <th>Count</th>
        </tr>
        {rows}
      </table>
    </div>
    """


# ============================================================
# Dashboard generation
# ============================================================

VIEW_OPTIONS = [
    ("overview", "Overview"),
    ("attributes", "Attributes"),
    ("timeline_thread", "Timeline: by thread"),
    ("timeline_thread_event", "Timeline: by thread + event"),
    ("timeline_depth", "Timeline: by BFS depth"),
    ("summary_event", "Summary: event type"),
    ("summary_depth", "Summary: BFS depth"),
    ("size_duration", "Scatter: size vs duration"),
    ("average_profile", "Average: timing profile"),
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


def unavailable_view(message: str) -> str:
    return f"""
    <div class="empty-view">
      <h2>Unavailable view</h2>
      <p>{html.escape(message)}</p>
    </div>
    """


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
            "attributes": build_attributes_html(run),
            "timeline_thread": build_timeline_thread_view(run),
            "timeline_thread_event": figure_to_div(
                build_timeline_figure(
                    spans,
                    title=f"Execution Timeline by Thread and Event Type — {run['label']}",
                    group_cols=("thread_id", "name"),
                    lane_fn=make_lane_by_thread_and_event,
                ),
                div_id=f"{run_id}_timeline_thread_event",
            ),
            "summary_event": figure_to_div(
                build_event_summary_figure(spans),
                div_id=f"{run_id}_summary_event",
            ),
            "average_profile": build_average_profile_html(run),
        }

        depth_timeline_fig = None
        if "depth" in spans.columns and spans["depth"].notna().any():
            depth_timeline_fig = build_timeline_figure(
                spans,
                title=f"Execution Timeline by BFS Depth — {run['label']}",
                group_cols=("depth",),
                lane_fn=make_lane_by_depth,
            )

        view_html["timeline_depth"] = (
            figure_to_div(depth_timeline_fig, div_id=f"{run_id}_timeline_depth")
            if depth_timeline_fig is not None
            else unavailable_view("This run has no depth information.")
        )

        depth_summary_fig = build_depth_summary_figure(spans)
        view_html["summary_depth"] = (
            figure_to_div(depth_summary_fig, div_id=f"{run_id}_summary_depth")
            if depth_summary_fig is not None
            else unavailable_view("This run has no depth information.")
        )

        size_fig = build_size_vs_duration_figure(spans)
        view_html["size_duration"] = (
            figure_to_div(size_fig, div_id=f"{run_id}_size_duration")
            if size_fig is not None
            else unavailable_view("This run has no row_size or frontier_size information.")
        )

        for view_id, _ in VIEW_OPTIONS:
            panels.append(f"""
            <section
              class="chart-panel"
              data-run="{html.escape(run_id)}"
              data-view="{html.escape(view_id)}"
            >
              {view_html[view_id]}
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

      .overview-card,
      .empty-view {
        background: white;
        color: #111827;
        border-radius: 12px;
        padding: 24px;
      }

      .overview-card h2,
      .overview-card h3,
      .empty-view h2 {
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

      .average-index-link,
      .average-report-link {
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

      .average-profile {
        background: #f8fafc;
        color: #111827;
        border-radius: 12px;
        padding: 22px;
      }

      .average-profile-heading {
        display: flex;
        justify-content: space-between;
        align-items: start;
        gap: 20px;
        flex-wrap: wrap;
      }

      .average-profile-heading h2,
      .average-profile-heading p {
        margin-top: 0;
      }

      .average-cards {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
        gap: 12px;
        margin: 18px 0;
      }

      .average-card {
        padding: 15px;
        border: 1px solid #dbe3ef;
        border-radius: 12px;
        background: white;
        box-shadow: 0 8px 24px rgba(15, 23, 42, 0.06);
      }

      .average-card span {
        display: block;
        color: #64748b;
        font-size: 12px;
        text-transform: uppercase;
        letter-spacing: 0.06em;
      }

      .average-card strong {
        display: block;
        margin-top: 7px;
        font-size: 21px;
      }

      .average-chart {
        overflow-x: auto;
        background: white;
        border-radius: 12px;
        border: 1px solid #e2e8f0;
      }

      .average-table-wrap {
        max-height: 680px;
        overflow: auto;
        border: 1px solid #dbe3ef;
        border-radius: 12px;
        background: white;
      }

      .average-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 13px;
      }

      .average-table th,
      .average-table td {
        padding: 9px 11px;
        border-bottom: 1px solid #e5e7eb;
        text-align: right;
        white-space: nowrap;
      }

      .average-table th:first-child,
      .average-table td:first-child {
        text-align: left;
      }

      .average-table thead {
        position: sticky;
        top: 0;
        background: #eef2f7;
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

      .attributes-view {
        background: #f8fafc;
        color: #111827;
        border-radius: 12px;
        padding: 22px;
      }

      .attributes-heading {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 18px;
        flex-wrap: wrap;
        margin-bottom: 16px;
      }

      .attributes-heading h2,
      .attributes-heading p {
        margin-top: 0;
      }

      .attribute-filter {
        min-width: min(420px, 100%);
        padding: 10px 12px;
        border: 1px solid #cbd5e1;
        border-radius: 9px;
        background: white;
        color: #111827;
      }

      .attribute-table-wrap {
        max-height: 760px;
        overflow: auto;
        border: 1px solid #dbe3ef;
        border-radius: 12px;
        background: white;
      }

      .attribute-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 13px;
      }

      .attribute-table th,
      .attribute-table td {
        padding: 9px 11px;
        border-bottom: 1px solid #e5e7eb;
        text-align: left;
        vertical-align: top;
      }

      .attribute-table thead {
        position: sticky;
        top: 0;
        background: #eef2f7;
      }

      .attribute-table code {
        white-space: pre-wrap;
        overflow-wrap: anywhere;
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

      function updateDashboard() {
        const run = document.getElementById("runSelect").value;
        const view = document.getElementById("viewSelect").value;

        document.querySelectorAll(".chart-panel").forEach(panel => {
          const active =
            panel.dataset.run === run &&
            panel.dataset.view === view;

          panel.classList.toggle("active", active);
        });

        setTimeout(() => {
          window.dispatchEvent(new Event("resize"));
        }, 50);
      }

      document.addEventListener("DOMContentLoaded", () => {
        document.querySelectorAll("[data-timeline-filter]").forEach(initializeTimelineFilter);
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
          <label>
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
                summary, average_events, report = load_average_artifacts(log_file)
            else:
                run_id = make_run_id(log_file, root)
                rel = log_file.relative_to(root) if log_file.is_relative_to(root) else log_file
                label = str(rel)
                summary, average_events, report = {}, pd.DataFrame(), None

            safe_id = make_safe_html_id(run_id)
            rel = log_file.relative_to(root) if log_file.is_relative_to(root) else log_file
            spans_csv = args.out_dir / "spans" / f"{safe_id}.spans.csv"
            spans_csv.parent.mkdir(parents=True, exist_ok=True)
            spans.drop(
                columns=["start_event", "end_event", "attributes"],
                errors="ignore",
            ).to_csv(spans_csv, index=False)
            report_href = (
                report.relative_to(args.out_dir).as_posix()
                if isinstance(report, Path) and report.is_file()
                else None
            )

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
                "average_summary": summary,
                "average_events": average_events,
                "average_report": report,
                "average_report_href": report_href,
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