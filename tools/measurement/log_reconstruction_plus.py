#!/usr/bin/env python3
"""Enhanced orchestration around the existing PolyGraph log_reconstruction.py.

Adds four workflow features without duplicating the existing dashboard renderer:

* --metrics-config CONFIG: export Prometheus metrics for server/request event ranges
  and build the metrics visualization in the same output tree.
* --split-by-run-type: build one reconstruction dashboard per benchmark run type.
* --compare-only: build only the compact Compare dashboard and skip reconstruction.
* --with-compare: additionally write a compact Compare-only page next to the normal
  reconstruction output.

This file can be used directly, or its small dispatch block can be copied into
log_reconstruction.py once the exact current revision is available.
"""

from __future__ import annotations

import argparse
import html
import os
import subprocess
import sys
from pathlib import Path
from typing import Sequence

PROJECT_ROOT = Path(__file__).resolve().parents[2]
BASE_RECONSTRUCTION = PROJECT_ROOT / "tools" / "measurement" / "log_reconstruction.py"

if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.measurement.compare_only import write_compare_dashboard
from tools.measurement.event_metrics import export_metrics, visualize_metrics


def _safe(value: str) -> str:
    import re
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value.strip()).strip(".-")
    return value or "run"


def _run(command: Sequence[object]) -> None:
    rendered = [str(item) for item in command]
    print("+ " + " ".join(rendered), flush=True)
    subprocess.run(rendered, cwd=PROJECT_ROOT, check=True)


def _find_event_roots(source: Path) -> list[tuple[str, Path]]:
    source = source.expanduser().resolve()
    if source.is_file():
        return [(source.parent.name, source)]
    candidates: list[tuple[str, Path]] = []
    # Canonical benchmark layout: <comparison>/<run-type>/events
    for events_dir in sorted(source.glob("*/events")):
        if events_dir.is_dir() and any(events_dir.rglob("*.jsonl")):
            candidates.append((events_dir.parent.name, events_dir))
    if candidates:
        return candidates
    if source.name == "events" and any(source.rglob("*.jsonl")):
        return [(source.parent.name, source)]
    # Last-resort discovery for one extra directory level.
    for events_dir in sorted(source.rglob("events")):
        if events_dir.is_dir() and any(events_dir.rglob("*.jsonl")):
            name = events_dir.parent.name
            candidates.append((name, events_dir))
    # Deduplicate paths.
    seen: set[Path] = set()
    result: list[tuple[str, Path]] = []
    for name, path in candidates:
        resolved = path.resolve()
        if resolved not in seen:
            seen.add(resolved)
            result.append((name, resolved))
    return result


def _base_command(args: argparse.Namespace, source: Path, output: Path) -> list[object]:
    command: list[object] = [sys.executable, BASE_RECONSTRUCTION, source, "--out-dir", output]
    if args.inline_plotly:
        command.append("--inline-plotly")
    if args.no_averages:
        command.append("--no-averages")
    if args.no_cache:
        command.append("--no-cache")
    if args.reorder_window is not None:
        command.extend(["--reorder-window", args.reorder_window])
    return command


def _find_dashboard(output: Path) -> Path | None:
    preferred = [output / "dashboard.html", output / "index.html"]
    for path in preferred:
        if path.is_file():
            return path
    dashboards = sorted(output.rglob("dashboard.html"))
    return dashboards[0] if dashboards else None


def _write_hub(output: Path, entries: list[tuple[str, str, Path]]) -> Path:
    cards = "".join(
        f'<a class="card" href="{html.escape(path.relative_to(output).as_posix())}">'
        f'<strong>{html.escape(label)}</strong><span>{html.escape(kind)}</span></a>'
        for label, kind, path in entries
        if path.is_file()
    )
    hub = output / "research_dashboard.html"
    hub.write_text(
        f"""<!doctype html><html><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/><title>PolyGraph measurement dashboard</title><style>
:root{{color-scheme:dark;--bg:#0b0f14;--card:#111821;--border:#263140;--text:#e5edf7;--muted:#93a4b8}}body{{margin:0;padding:28px;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,sans-serif}}.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:12px}}.card{{display:flex;flex-direction:column;gap:5px;padding:16px;border:1px solid var(--border);border-radius:12px;background:var(--card);color:var(--text);text-decoration:none}}.card span{{color:var(--muted)}}
</style></head><body><h1>PolyGraph measurement dashboard</h1><div class="grid">{cards}</div></body></html>""",
        encoding="utf-8",
    )
    return hub


def _metrics_for_source(source: Path, config: Path, output: Path, *, overwrite: bool) -> tuple[Path, Path]:
    metrics_root = output / "prometheus"
    existing_manifest = metrics_root / "manifest.json"
    if existing_manifest.is_file() and not overwrite:
        print(f"Reusing Prometheus export: {existing_manifest}", flush=True)
        manifest = existing_manifest
    else:
        manifest = export_metrics(source, config, metrics_root, overwrite=overwrite)
    dashboard = visualize_metrics(
        manifest,
        output_dir=metrics_root / "visualization",
        config_path=config,
    )
    return manifest, dashboard


def run_single(args: argparse.Namespace, source: Path, output: Path) -> list[tuple[str, str, Path]]:
    output.mkdir(parents=True, exist_ok=True)
    entries: list[tuple[str, str, Path]] = []

    if args.compare_only:
        compare = write_compare_dashboard(
            source,
            output / "compare.html",
            include_server=args.include_server_in_compare,
        )
        entries.append((source.name, "Compare only", compare))
        if args.metrics_config:
            _, metrics_dashboard = _metrics_for_source(
                source,
                args.metrics_config,
                output,
                overwrite=args.refresh_metrics,
            )
            entries.append((source.name, "Prometheus metrics", metrics_dashboard))
        return entries

    _run(_base_command(args, source, output))
    dashboard = _find_dashboard(output)
    if dashboard:
        entries.append((source.name, "Event reconstruction", dashboard))

    if args.with_compare:
        compare = write_compare_dashboard(
            source,
            output / "compare.html",
            include_server=args.include_server_in_compare,
        )
        entries.append((source.name, "Compare only", compare))

    if args.metrics_config:
        _, metrics_dashboard = _metrics_for_source(
            source,
            args.metrics_config,
            output,
            overwrite=args.refresh_metrics,
        )
        entries.append((source.name, "Prometheus metrics", metrics_dashboard))
    return entries


def run_split(args: argparse.Namespace, source: Path, output: Path) -> Path:
    roots = _find_event_roots(source)
    if not roots:
        raise ValueError(f"no run-type event directories found under {source}")
    output.mkdir(parents=True, exist_ok=True)
    entries: list[tuple[str, str, Path]] = []
    for index, (name, event_root) in enumerate(roots, start=1):
        print(f"\n=== [{index}/{len(roots)}] {name} ===", flush=True)
        run_output = output / "runs" / _safe(name)
        run_entries = run_single(args, event_root, run_output)
        for _, kind, path in run_entries:
            entries.append((name, kind, path))

    # The global compare should compare all run types together, not one at a time.
    if args.with_compare or args.compare_only:
        compare = write_compare_dashboard(
            source,
            output / "compare.html",
            include_server=args.include_server_in_compare,
        )
        entries.insert(0, ("All run types", "Compare", compare))

    hub = _write_hub(output, entries)
    return hub


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, help="Measurement tree or event-log directory.")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--inline-plotly", action="store_true")
    parser.add_argument("--no-averages", action="store_true")
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument("--reorder-window", type=int)

    parser.add_argument(
        "--metrics-config",
        type=Path,
        help="Export Prometheus metrics for every discovered server/request event range and generate metric pages.",
    )
    parser.add_argument(
        "--refresh-metrics",
        action="store_true",
        help="Overwrite an existing Prometheus export instead of reusing/protecting it.",
    )
    parser.add_argument(
        "--split-by-run-type",
        action="store_true",
        help="Generate one reconstruction directory/dashboard per benchmark run type.",
    )
    parser.add_argument(
        "--compare-only",
        action="store_true",
        help="Skip normal reconstruction and generate only the compact Compare page.",
    )
    parser.add_argument(
        "--with-compare",
        action="store_true",
        help="Generate compact compare.html in addition to the normal dashboard.",
    )
    parser.add_argument(
        "--include-server-in-compare",
        action="store_true",
        help="Allow server.events.jsonl samples in the compact comparison input (still visually separate).",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    source = args.logs.expanduser().resolve()
    output = args.out_dir.expanduser().resolve()
    try:
        if args.split_by_run_type:
            hub = run_split(args, source, output)
            print(f"\nDashboard hub: {hub}")
            return 0
        entries = run_single(args, source, output)
        hub = _write_hub(output, entries)
        print(f"Dashboard hub: {hub}")
        return 0
    except subprocess.CalledProcessError as exc:
        print(f"log_reconstruction_plus: base reconstruction failed with exit code {exc.returncode}", file=sys.stderr)
        return exc.returncode or 1
    except Exception as exc:  # noqa: BLE001
        print(f"log_reconstruction_plus: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
