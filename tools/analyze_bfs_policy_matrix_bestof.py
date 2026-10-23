#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any

POLICIES = ("classic", "direction-optimized")


def read_row(run_root: Path) -> dict[str, Any] | None:
    path = run_root / "comparison.json"
    if not path.is_file():
        return None
    rows = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(rows, list) or len(rows) != 1 or not isinstance(rows[0], dict):
        raise ValueError(f"Expected one comparison row in {path}")
    return dict(rows[0])


def event_counts(run_root: Path) -> dict[str, int]:
    counts = {
        "fallback_events": 0,
        "direction_decisions": 0,
        "pull_decisions": 0,
        "bitmap_pull_decisions": 0,
    }
    for path in run_root.rglob("server.events.jsonl"):
        try:
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                if "full_bfs.direction_optimized_fallback" in line:
                    counts["fallback_events"] += 1
                if "bfs.direction_decision" in line:
                    counts["direction_decisions"] += 1
                    if '"direction":"pull"' in line or '"direction": "pull"' in line:
                        counts["pull_decisions"] += 1
                        if '"frontier_representation":"bitmap"' in line or '"frontier_representation": "bitmap"' in line:
                            counts["bitmap_pull_decisions"] += 1
        except OSError:
            pass
    return counts


def number(value: Any) -> float | None:
    if value is None or value == "":
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def fmt_ms(value: float | None) -> str:
    if value is None:
        return "n/a"
    if value >= 1000:
        return f"{value / 1000.0:.3f} s"
    return f"{value:.3f} ms"


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare classic and best-of-both direction-optimized BFS runs.")
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    out = root / "_analysis"
    out.mkdir(parents=True, exist_ok=True)

    names: set[str] = set()
    for policy in POLICIES:
        pdir = root / policy
        if pdir.is_dir():
            names.update(p.name for p in pdir.iterdir() if p.is_dir())

    details: list[dict[str, Any]] = []
    by_key: dict[tuple[str, str], dict[str, Any]] = {}
    for policy in POLICIES:
        for name in sorted(names):
            run_root = root / policy / name
            row = read_row(run_root)
            counts = event_counts(run_root)
            item: dict[str, Any] = {
                "policy": policy,
                "cache": name,
                "status": "missing" if row is None else str(row.get("status", "unknown")),
                "mean_server_bfs_ms": None if row is None else number(row.get("mean_server_bfs_ms")),
                "mean_http_bfs_ms": None if row is None else number(row.get("mean_bfs_ms")),
                "successful_runs": 0 if row is None else int(row.get("successful_runs") or 0),
                "failed_runs": 0 if row is None else int(row.get("failed_runs") or 0),
                **counts,
            }
            details.append(item)
            by_key[(policy, name)] = item

    comparisons: list[dict[str, Any]] = []
    for name in sorted(names):
        classic = by_key.get(("classic", name), {})
        opt = by_key.get(("direction-optimized", name), {})
        c = classic.get("mean_server_bfs_ms")
        o = opt.get("mean_server_bfs_ms")
        speedup = c / o if isinstance(c, (int, float)) and isinstance(o, (int, float)) and o > 0 else None
        reduction = (1.0 - o / c) * 100.0 if isinstance(c, (int, float)) and isinstance(o, (int, float)) and c > 0 else None
        comparisons.append({
            "cache": name,
            "classic_server_bfs_ms": c,
            "optimized_server_bfs_ms": o,
            "speedup_classic_over_optimized": speedup,
            "optimized_reduction_percent": reduction,
            "optimized_fallback_events": opt.get("fallback_events", 0),
            "optimized_pull_decisions": opt.get("pull_decisions", 0),
            "optimized_bitmap_pull_decisions": opt.get("bitmap_pull_decisions", 0),
        })

    with (out / "runs.csv").open("w", newline="", encoding="utf-8") as f:
        fields = list(details[0].keys()) if details else ["policy", "cache"]
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(details)
    with (out / "comparison.csv").open("w", newline="", encoding="utf-8") as f:
        fields = list(comparisons[0].keys()) if comparisons else ["cache"]
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(comparisons)
    (out / "summary.json").write_text(json.dumps({"runs": details, "comparisons": comparisons}, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# BFS policy comparison",
        "",
        "`direction-optimized` now delegates to the classic executor when no complete pull backend exists.",
        "A speedup above 1.0 means the optimized policy is faster.",
        "",
        "| Cache | Classic server BFS | Optimized server BFS | Speedup | Reduction | Fallback events | Pull decisions | Bitmap pulls |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in comparisons:
        speedup = row["speedup_classic_over_optimized"]
        reduction = row["optimized_reduction_percent"]
        lines.append(
            f"| {row['cache']} | {fmt_ms(row['classic_server_bfs_ms'])} | {fmt_ms(row['optimized_server_bfs_ms'])} | "
            f"{('n/a' if speedup is None else f'{speedup:.3f}x')} | "
            f"{('n/a' if reduction is None else f'{reduction:.1f}%')} | "
            f"{row['optimized_fallback_events']} | {row['optimized_pull_decisions']} | {row['optimized_bitmap_pull_decisions']} |"
        )
    lines += ["", "Raw run data: `runs.csv`; pairwise comparison: `comparison.csv`."]
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
