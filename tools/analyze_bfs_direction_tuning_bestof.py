#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


def main() -> int:
    parser = argparse.ArgumentParser(description="Rank MMap100 direction-optimized threshold variants by mean server BFS time.")
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    out = root / "_analysis"
    out.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, Any]] = []
    for run in sorted(p for p in root.iterdir() if p.is_dir() and not p.name.startswith("_" ) and p.name != "logs"):
        comparison = run / "comparison.json"
        if not comparison.is_file():
            rows.append({"variant": run.name, "status": "missing", "mean_server_bfs_ms": None})
            continue
        values = json.loads(comparison.read_text(encoding="utf-8"))
        row = values[0] if isinstance(values, list) and values else {}
        rows.append({
            "variant": run.name,
            "status": row.get("status"),
            "mean_server_bfs_ms": row.get("mean_server_bfs_ms"),
            "mean_http_bfs_ms": row.get("mean_bfs_ms"),
            "successful_runs": row.get("successful_runs"),
            "failed_runs": row.get("failed_runs"),
        })

    successful = [r for r in rows if r.get("mean_server_bfs_ms") not in (None, "")]
    successful.sort(key=lambda r: float(r["mean_server_bfs_ms"]))
    rank = {r["variant"]: i + 1 for i, r in enumerate(successful)}
    for row in rows:
        row["rank"] = rank.get(row["variant"])

    fields = ["rank", "variant", "status", "mean_server_bfs_ms", "mean_http_bfs_ms", "successful_runs", "failed_runs"]
    with (out / "ranking.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader(); writer.writerows(sorted(rows, key=lambda r: r.get("rank") or 10**9))
    (out / "ranking.json").write_text(json.dumps(sorted(rows, key=lambda r: r.get("rank") or 10**9), indent=2) + "\n", encoding="utf-8")

    lines = ["# Direction-optimized MMap100 tuning", "", "| Rank | Variant | Mean server BFS | Status |", "|---:|---|---:|---|"]
    for row in sorted(rows, key=lambda r: r.get("rank") or 10**9):
        value = row.get("mean_server_bfs_ms")
        shown = "n/a" if value in (None, "") else f"{float(value) / 1000.0:.3f} s"
        lines.append(f"| {row.get('rank') or '-'} | {row['variant']} | {shown} | {row.get('status', '')} |")
    if successful:
        lines += ["", f"Best measured variant: **{successful[0]['variant']}** ({float(successful[0]['mean_server_bfs_ms']) / 1000.0:.3f} s mean server BFS)."]
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
