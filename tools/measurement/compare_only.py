#!/usr/bin/env python3
"""Generate a lightweight Compare-only HTML from PolyGraph event logs.

This deliberately does not build the normal per-run dashboard. It reads only the
event JSONL files, reconstructs completed spans, stores compact per-request timing
metadata in the HTML, and renders comparison boxplots in the browser.

The default sample source is events/requests/*.events.jsonl. server.events.jsonl
can be included explicitly, but is off by default because it spans the whole server
lifetime rather than a single BFS request.
"""

from __future__ import annotations

import argparse
import html
import json
import math
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


@dataclass(frozen=True)
class Span:
    name: str
    start_ns: int
    end_ns: int

    @property
    def duration_ms(self) -> float:
        return max(0.0, (self.end_ns - self.start_ns) / 1_000_000.0)


@dataclass(frozen=True)
class Sample:
    run_type: str
    source_kind: str
    label: str
    warmup: bool
    path: Path
    families: Mapping[str, float]
    events: Mapping[str, float]


FAMILY_MATCHERS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("full_bfs", ("full_bfs", "full-bfs", "bfs.total", "bfs_total")),
    ("neo4j", ("neo4j",)),
    ("arango", ("arangodb", "arango")),
    ("cugraph", ("cugraph",)),
    ("gpu", ("gpu", "cuda")),
    ("redis", ("redis",)),
    ("mmap", ("mmap", "memory_map", "memory-map")),
    ("postgresql", ("postgresql", "postgres")),
    ("backend", ("backend", "scheduler")),
    ("bfs", ("bfs",)),
)

FAMILY_LABELS = {
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


def _safe(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value.strip()).strip(".-")
    return value or "run"


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(row, dict):
                rows.append(row)
    return rows


def _timestamp_ns(row: Mapping[str, Any]) -> int | None:
    for key in ("steady_ns", "elapsed_ns", "timestamp_unix_ns"):
        value = row.get(key)
        if value is None:
            continue
        try:
            return int(value)
        except (TypeError, ValueError):
            continue
    return None


def _edge(name: str) -> tuple[str, str | None]:
    if name.endswith(".start"):
        return name[:-6], "start"
    if name.endswith(".end"):
        return name[:-4], "end"
    return name, None


def reconstruct_spans(path: Path) -> list[Span]:
    stacks: dict[tuple[str, str], list[int]] = defaultdict(list)
    spans: list[Span] = []
    for row in _read_jsonl(path):
        name = str(row.get("event", ""))
        base, edge = _edge(name)
        if edge is None:
            continue
        timestamp = _timestamp_ns(row)
        if timestamp is None:
            continue
        thread = str(row.get("thread_id", row.get("threadId", "")))
        key = (thread, base)
        if edge == "start":
            stacks[key].append(timestamp)
        elif stacks[key]:
            start = stacks[key].pop()
            if timestamp >= start:
                spans.append(Span(base, start, timestamp))
    return spans


def _union_ms(spans: Iterable[Span]) -> float:
    intervals = sorted((span.start_ns, span.end_ns) for span in spans if span.end_ns >= span.start_ns)
    if not intervals:
        return 0.0
    total = 0
    start, end = intervals[0]
    for next_start, next_end in intervals[1:]:
        if next_start <= end:
            end = max(end, next_end)
        else:
            total += end - start
            start, end = next_start, next_end
    total += end - start
    return total / 1_000_000.0


def _family_for_event(name: str) -> str:
    lowered = name.lower()
    for family, needles in FAMILY_MATCHERS:
        if any(needle in lowered for needle in needles):
            return family
    # Preserve a stable generic family for otherwise-unclassified hierarchical events.
    token = re.split(r"[._:/-]+", lowered, maxsplit=1)[0]
    return token or "other"


def _run_type(path: Path, root: Path) -> str:
    events_ancestor = next((parent for parent in path.parents if parent.name == "events"), None)
    if events_ancestor is not None:
        parent = events_ancestor.parent
        try:
            rel = parent.relative_to(root)
            if rel.parts:
                return rel.parts[-1]
        except ValueError:
            pass
        return parent.name
    try:
        rel = path.relative_to(root)
        return rel.parts[0] if len(rel.parts) > 1 else root.name
    except ValueError:
        return path.parent.name


def _discover_files(root: Path, *, include_server: bool) -> list[tuple[Path, str]]:
    request_files = {p.resolve() for p in root.rglob("events/requests/*.events.jsonl")}
    if root.name == "events":
        request_files.update(p.resolve() for p in (root / "requests").glob("*.events.jsonl"))
    results = [(path, "request") for path in sorted(request_files)]
    # If there are no canonical request logs, keep generic event logs usable too.
    if not results:
        for path in sorted(root.rglob("*.events.jsonl")):
            if path.name == "server.events.jsonl":
                continue
            results.append((path, "event"))
    if include_server:
        server_files = {p.resolve() for p in root.rglob("events/server.events.jsonl")}
        if root.name == "events" and (root / "server.events.jsonl").is_file():
            server_files.add((root / "server.events.jsonl").resolve())
        results.extend((path, "server") for path in sorted(server_files))
    # Deduplicate while retaining order.
    seen: set[Path] = set()
    unique: list[tuple[Path, str]] = []
    for path, kind in results:
        resolved = path.resolve()
        if resolved not in seen:
            seen.add(resolved)
            unique.append((resolved, kind))
    return unique


def load_samples(source: Path, *, include_server: bool = False) -> list[Sample]:
    root = source.expanduser().resolve()
    if root.is_file():
        files = [(root, "server" if root.name == "server.events.jsonl" else "event")]
        root = root.parent
    else:
        files = _discover_files(root, include_server=include_server)
    samples: list[Sample] = []
    for path, source_kind in files:
        spans = reconstruct_spans(path)
        if not spans:
            continue
        by_event: dict[str, list[Span]] = defaultdict(list)
        by_family: dict[str, list[Span]] = defaultdict(list)
        for span in spans:
            by_event[span.name].append(span)
            by_family[_family_for_event(span.name)].append(span)
        events = {name: _union_ms(group) for name, group in by_event.items()}
        families = {name: _union_ms(group) for name, group in by_family.items()}
        label = path.name.removesuffix(".events.jsonl")
        samples.append(
            Sample(
                run_type=_run_type(path, root),
                source_kind=source_kind,
                label=label,
                warmup=label.lower().startswith("warmup-"),
                path=path,
                families=families,
                events=events,
            )
        )
    return samples


def _safe_json(value: Any) -> str:
    return json.dumps(value, separators=(",", ":")).replace("</", "<\\/")


def write_compare_dashboard(
    source: Path,
    output: Path,
    *,
    include_server: bool = False,
    plotly_cdn: str = "https://cdn.plot.ly/plotly-2.35.2.min.js",
) -> Path:
    samples = load_samples(source, include_server=include_server)
    if not samples:
        raise ValueError(f"no comparable event samples found under {source}")

    all_families = sorted({name for sample in samples for name in sample.families}, key=str.lower)
    all_events = sorted({name for sample in samples for name in sample.events}, key=str.lower)
    metrics: list[dict[str, str]] = []
    for name in all_families:
        metrics.append({"key": f"family:{name}", "kind": "family", "name": name, "label": FAMILY_LABELS.get(name, name)})
    for name in all_events:
        metrics.append({"key": f"event:{name}", "kind": "event", "name": name, "label": name})
    preferred = ["event:full_bfs", "family:full_bfs", "event:bfs", "family:bfs"]
    keys = {metric["key"] for metric in metrics}
    default_metric = next((key for key in preferred if key in keys), metrics[0]["key"])

    payload = {
        "samples": [
            {
                "runType": sample.run_type,
                "sourceKind": sample.source_kind,
                "label": sample.label,
                "warmup": sample.warmup,
                "path": str(sample.path),
                "families": sample.families,
                "events": sample.events,
            }
            for sample in samples
        ],
        "metrics": metrics,
        "defaultMetric": default_metric,
        "runTypes": sorted({sample.run_type for sample in samples}, key=str.lower),
    }

    options = "".join(
        f'<option value="{html.escape(metric["key"])}"'
        f'{" selected" if metric["key"] == default_metric else ""}>{html.escape(metric["label"])}</option>'
        for metric in metrics
    )
    run_checks = "".join(
        f'<label><input type="checkbox" data-run-type value="{html.escape(run)}" checked/> {html.escape(run)}</label>'
        for run in payload["runTypes"]
    )
    source_checks = (
        '<label><input type="checkbox" data-source value="request" checked/> request logs</label>'
        '<label><input type="checkbox" data-source value="event" checked/> other event logs</label>'
        + ('<label><input type="checkbox" data-source value="server"/> server.events.jsonl</label>' if include_server else '')
    )

    output = output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        f"""<!doctype html><html><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>PolyGraph Compare</title><script src="{html.escape(plotly_cdn)}"></script><style>
:root{{color-scheme:dark;--bg:#0b0f14;--card:#111821;--border:#263140;--text:#e5edf7;--muted:#93a4b8}}*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,sans-serif}}header{{padding:18px 22px;border-bottom:1px solid var(--border);position:sticky;top:0;background:#0b0f14ee;z-index:4}}h1{{margin:0 0 4px}}header p{{margin:0;color:var(--muted)}}main{{display:grid;grid-template-columns:280px minmax(0,1fr);gap:16px;padding:16px}}aside,.card{{background:var(--card);border:1px solid var(--border);border-radius:12px}}aside{{padding:14px;align-self:start;position:sticky;top:100px;max-height:calc(100vh - 116px);overflow:auto}}aside h3{{margin:16px 0 7px;font-size:13px}}aside h3:first-child{{margin-top:0}}aside label{{display:block;padding:3px 0}}select{{width:100%;background:#0d141d;color:var(--text);border:1px solid var(--border);border-radius:7px;padding:8px}}.content{{display:grid;gap:14px}}.card{{padding:12px}}#boxplot{{height:560px}}table{{width:100%;border-collapse:collapse}}th,td{{padding:8px;border-bottom:1px solid var(--border);text-align:right}}th:first-child,td:first-child{{text-align:left}}.muted{{color:var(--muted)}}@media(max-width:850px){{main{{grid-template-columns:1fr}}aside{{position:static;max-height:none}}}}
</style></head><body><header><h1>Compare run configurations</h1><p>Compact comparison only: no per-run timelines or full dashboard figures are embedded.</p></header><main>
<aside><h3>Primary metric</h3><select id="metric">{options}</select><h3>Run configurations</h3><div id="runTypes">{run_checks}</div><h3>Sample sources</h3><div>{source_checks}</div><h3>Warmup</h3><label><input type="checkbox" id="includeWarmup"/> Include warmup requests</label><h3>Order</h3><select id="sort"><option value="name">Name</option><option value="median">Fastest median</option></select><h3>Y scale</h3><select id="scale"><option value="linear">Linear</option><option value="log">Logarithmic</option></select></aside>
<div class="content"><section class="card"><div id="boxplot"></div></section><section class="card"><table><thead><tr><th>Run configuration</th><th>n</th><th>Median [ms]</th><th>IQR [ms]</th><th>Min</th><th>Max</th><th>vs best</th></tr></thead><tbody id="summary"></tbody></table></section></div>
</main><script>const data={_safe_json(payload)};
const metric=document.getElementById('metric'), sort=document.getElementById('sort'), scale=document.getElementById('scale'), summary=document.getElementById('summary'), includeWarmup=document.getElementById('includeWarmup');
const checked=s=>new Set([...document.querySelectorAll(s+':checked')].map(x=>x.value));
const quantile=(a,q)=>{{if(!a.length)return NaN;const b=[...a].sort((x,y)=>x-y),p=(b.length-1)*q,i=Math.floor(p),f=p-i;return b[i+1]===undefined?b[i]:b[i]+f*(b[i+1]-b[i]);}};
function render(){{const runs=checked('[data-run-type]'),sources=checked('[data-source]'),showWarmup=includeWarmup.checked;const [kind,name]=metric.value.split(':',2);let groups=[];for(const run of data.runTypes){{if(!runs.has(run))continue;const values=data.samples.filter(s=>s.runType===run&&sources.has(s.sourceKind)&&(showWarmup||!s.warmup)).map(s=>Number((kind==='family'?s.families:s.events)[name])).filter(Number.isFinite);if(values.length)groups.push({{run,values,median:quantile(values,.5),q1:quantile(values,.25),q3:quantile(values,.75),min:Math.min(...values),max:Math.max(...values)}});}}if(sort.value==='median')groups.sort((a,b)=>a.median-b.median);else groups.sort((a,b)=>a.run.localeCompare(b.run));const traces=groups.map(g=>({{type:'box',name:g.run,y:g.values,boxpoints:'all',jitter:.22,pointpos:0,hovertemplate:g.run+'<br>%{{y:.3f}} ms<extra></extra>'}}));Plotly.react('boxplot',traces,{{margin:{{l:70,r:20,t:25,b:110}},paper_bgcolor:'transparent',plot_bgcolor:'transparent',font:{{color:'#dce6f2'}},showlegend:false,yaxis:{{title:'Active wall-clock time [ms]',type:scale.value,gridcolor:'#243041'}},xaxis:{{tickangle:-30}}}},{{responsive:true,displaylogo:false}});const best=Math.min(...groups.map(g=>g.median));summary.innerHTML=groups.map(g=>`<tr><td>${{g.run}}</td><td>${{g.values.length}}</td><td>${{g.median.toFixed(3)}}</td><td>${{(g.q3-g.q1).toFixed(3)}}</td><td>${{g.min.toFixed(3)}}</td><td>${{g.max.toFixed(3)}}</td><td>${{best>0?(g.median/best).toFixed(2)+'×':'—'}}</td></tr>`).join('');}}
document.querySelectorAll('input,select').forEach(x=>x.addEventListener('change',render));render();</script></body></html>""",
        encoding="utf-8",
    )
    return output


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--include-server", action="store_true")
    parser.add_argument("--plotly-cdn", default="https://cdn.plot.ly/plotly-2.35.2.min.js")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        output = write_compare_dashboard(args.source, args.out, include_server=args.include_server, plotly_cdn=args.plotly_cdn)
        print(f"Compare dashboard: {output}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"compare_only: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
