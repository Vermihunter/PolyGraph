#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def as_float(value: object) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def load_meta(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    if not path.is_file():
        return result
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key] = value
    return result


def comparison_row(path: Path) -> dict[str, object] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(data, list) or not data or not isinstance(data[0], dict):
        return None
    return data[0]


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze fixed-thread Arango chunk-size sweep results.")
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    root = Path(args.root).expanduser().resolve()
    analysis = root / "_analysis"
    analysis.mkdir(parents=True, exist_ok=True)
    meta = load_meta(root / "sweep.env")

    per_run: list[dict[str, object]] = []
    for pass_dir in sorted(root.glob("pass-*")):
        if not pass_dir.is_dir():
            continue
        for chunk_dir in sorted(pass_dir.glob("chunk-*")):
            if not chunk_dir.is_dir():
                continue
            try:
                chunk_size = int(chunk_dir.name.removeprefix("chunk-"))
            except ValueError:
                continue
            for cache_dir in sorted(p for p in chunk_dir.iterdir() if p.is_dir()):
                item = comparison_row(cache_dir / "comparison.json")
                if item is None:
                    per_run.append({
                        "pass": pass_dir.name,
                        "chunk_size": chunk_size,
                        "cache_name": cache_dir.name,
                        "status": "missing",
                        "mean_bfs_ms": None,
                        "mean_server_bfs_ms": None,
                    })
                    continue
                per_run.append({
                    "pass": pass_dir.name,
                    "chunk_size": chunk_size,
                    "cache_name": str(item.get("cache_name") or cache_dir.name),
                    "status": str(item.get("status", "")),
                    "mean_bfs_ms": as_float(item.get("mean_bfs_ms")),
                    "mean_server_bfs_ms": as_float(item.get("mean_server_bfs_ms")),
                })

    per_run_fields = ["pass", "chunk_size", "cache_name", "status", "mean_bfs_ms", "mean_server_bfs_ms"]
    with (analysis / "per_run.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=per_run_fields)
        writer.writeheader()
        writer.writerows(per_run)

    grouped: dict[tuple[str, int], list[dict[str, object]]] = defaultdict(list)
    for row in per_run:
        if row["status"] == "success" and row["mean_server_bfs_ms"] is not None:
            grouped[(str(row["cache_name"]), int(row["chunk_size"]))].append(row)

    aggregate: list[dict[str, object]] = []
    for (cache, chunk), rows in sorted(grouped.items()):
        server_values = [float(r["mean_server_bfs_ms"]) for r in rows if r["mean_server_bfs_ms"] is not None]
        http_values = [float(r["mean_bfs_ms"]) for r in rows if r["mean_bfs_ms"] is not None]
        aggregate.append({
            "cache_name": cache,
            "chunk_size": chunk,
            "successful_passes": len(server_values),
            "mean_server_bfs_ms": statistics.fmean(server_values),
            "median_server_bfs_ms": statistics.median(server_values),
            "min_server_bfs_ms": min(server_values),
            "max_server_bfs_ms": max(server_values),
            "mean_http_bfs_ms": statistics.fmean(http_values) if http_values else None,
        })

    by_cache: dict[str, list[dict[str, object]]] = defaultdict(list)
    for row in aggregate:
        by_cache[str(row["cache_name"])].append(row)

    for cache, rows in by_cache.items():
        best = min(rows, key=lambda r: float(r["mean_server_bfs_ms"]))
        baseline = next((r for r in rows if int(r["chunk_size"]) == 250000), None)
        for row in rows:
            row["best_chunk_size"] = int(best["chunk_size"])
            row["relative_to_best"] = float(row["mean_server_bfs_ms"]) / float(best["mean_server_bfs_ms"])
            row["speedup_vs_250k"] = (
                float(baseline["mean_server_bfs_ms"]) / float(row["mean_server_bfs_ms"])
                if baseline is not None and float(row["mean_server_bfs_ms"]) > 0
                else None
            )

    aggregate_fields = [
        "cache_name", "chunk_size", "successful_passes",
        "mean_server_bfs_ms", "median_server_bfs_ms", "min_server_bfs_ms", "max_server_bfs_ms",
        "mean_http_bfs_ms", "best_chunk_size", "relative_to_best", "speedup_vs_250k",
    ]
    with (analysis / "aggregate.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=aggregate_fields)
        writer.writeheader()
        writer.writerows({field: row.get(field, "") for field in aggregate_fields} for row in aggregate)

    lines = [
        "# Arango chunk-size sweep",
        "",
        f"- Arango scheduler threads: **{meta.get('threads', '?')}** (minimal=maximal)",
        f"- PolyGraph Arango max concurrent requests: **{meta.get('arango_max_concurrent_requests', '?')}**",
        "- Arango access: **edge-index + AQL plan cache**",
        "- Request strategy: **chunked**",
        f"- Repetitions per run: **{meta.get('repetitions', '?')}**",
        f"- Passes: **{meta.get('passes', '?')}**",
        "",
        "Lower server BFS time is better.",
        "",
        "| Cache | Chunk | Passes | Server BFS mean (ms) | Median (ms) | Speedup vs 250k |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for row in sorted(aggregate, key=lambda r: (str(r["cache_name"]), int(r["chunk_size"]))):
        speedup = row.get("speedup_vs_250k")
        lines.append(
            f"| {row['cache_name']} | {int(row['chunk_size']):,} | {row['successful_passes']} | "
            f"{float(row['mean_server_bfs_ms']):.3f} | {float(row['median_server_bfs_ms']):.3f} | "
            f"{'n/a' if speedup is None else f'{float(speedup):.3f}x'} |"
        )

    if by_cache:
        lines += ["", "## Best chunk per cache", ""]
        for cache in sorted(by_cache):
            best = min(by_cache[cache], key=lambda r: float(r["mean_server_bfs_ms"]))
            lines.append(
                f"- `{cache}`: **{int(best['chunk_size']):,}** vertices/request, "
                f"mean server BFS **{float(best['mean_server_bfs_ms']):.3f} ms**"
            )

    missing = [r for r in per_run if r["status"] != "success"]
    if missing:
        lines += ["", "## Incomplete runs", ""]
        for row in missing:
            lines.append(f"- `{row['pass']}` chunk `{row['chunk_size']}` / `{row['cache_name']}`: {row['status']}")

    (analysis / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (analysis / "summary.json").write_text(
        json.dumps({"meta": meta, "per_run": per_run, "aggregate": aggregate}, indent=2) + "\n",
        encoding="utf-8",
    )
    print(analysis / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
