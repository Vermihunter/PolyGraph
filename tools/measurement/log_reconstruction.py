#!/usr/bin/env python3

import argparse
import html
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

import pandas as pd
import plotly.graph_objects as go


PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.event_log_average import AVERAGE_DIR_NAME, analyze_log_tree


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

            duration_ns = end_ns - start_ns

            spans.append({
                "name": base_name,
                "thread_id": thread_id,

                "seq_start": start_row.get("seq"),
                "seq_end": end_row.get("seq"),

                "start_ns": start_ns,
                "end_ns": end_ns,

                "duration_ns": duration_ns,
                "duration_us_calc": duration_ns / 1_000.0,
                "duration_ms": duration_ns / 1_000_000.0,

                "duration_us_logged": merged.get("duration_us"),

                "depth": merged.get("depth"),
                "row_size": merged.get("row_size"),
                "frontier_size": merged.get("frontier_size"),

                "start_event": start_row,
                "end_event": end_row,
            })

    unclosed_count = sum(len(v) for v in stacks.values())
    unmatched_end_count = len(unmatched_ends)

    if not spans:
        raise ValueError("No spans reconstructed. Make sure events use .start and .end suffixes.")

    span_df = pd.DataFrame(spans)

    t0 = span_df["start_ns"].min()

    span_df["start_ms"] = (span_df["start_ns"] - t0) / 1_000_000.0
    span_df["end_ms"] = (span_df["end_ns"] - t0) / 1_000_000.0

    span_df.attrs["unclosed_count"] = unclosed_count
    span_df.attrs["unmatched_end_count"] = unmatched_end_count

    return span_df.sort_values(["start_ns", "end_ns"]).reset_index(drop=True)


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
    parts = [
        f"<b>{html.escape(str(row['name']))}</b>",
        f"thread: {row['thread_id']}",
        f"lane: {int(row['overlap_lane'])}",
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

    return "<br>".join(parts)


def make_lane_by_thread(row) -> str:
    return f"thread {row['thread_id']} / lane {int(row['overlap_lane'])}"


def make_lane_by_thread_and_event(row) -> str:
    return f"thread {row['thread_id']} / {row['name']} / lane {int(row['overlap_lane'])}"


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
        fig.add_trace(go.Bar(
            x=group["duration_ms"],
            y=group["lane"],
            base=group["start_ms"],
            orientation="h",
            name=event_name,
            customdata=group[["hover"]],
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
            "timeline_thread": figure_to_div(
                build_timeline_figure(
                    spans,
                    title=f"Execution Timeline by Thread — {run['label']}",
                    group_cols=("thread_id",),
                    lane_fn=make_lane_by_thread,
                ),
                div_id=f"{run_id}_timeline_thread",
            ),
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
    </style>
    """

    js = """
    <script>
      function updateDashboard() {
        const run = document.getElementById("runSelect").value;
        const view = document.getElementById("viewSelect").value;

        document.querySelectorAll(".chart-panel").forEach(panel => {
          const active =
            panel.dataset.run === run &&
            panel.dataset.view === view;

          panel.classList.toggle("active", active);
        });

        // Helps Plotly resize after switching hidden panels.
        setTimeout(() => {
          window.dispatchEvent(new Event("resize"));
        }, 50);
      }

      document.addEventListener("DOMContentLoaded", () => {
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
            spans.drop(columns=["start_event", "end_event"]).to_csv(spans_csv, index=False)
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