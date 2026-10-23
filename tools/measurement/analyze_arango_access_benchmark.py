#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def load_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value
    return values


def as_float(value: object) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize Arango A0/A1 benchmark variants.")
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    root = Path(args.root).expanduser().resolve()
    analysis = root / "_analysis"
    analysis.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, object]] = []
    for variant_dir in sorted(p for p in root.iterdir() if p.is_dir() and not p.name.startswith("_")):
        meta = load_env(variant_dir / "arango-access.env")
        comparison_path = variant_dir / "comparison.json"
        if not comparison_path.is_file():
            rows.append({
                "variant": variant_dir.name,
                "variant_status": meta.get("status", "missing-results"),
                "cache_name": "",
                "cache_status": "",
                "mean_bfs_ms": None,
                "mean_server_bfs_ms": None,
                "a0_batch_size": meta.get("arango_traversal_batch_size", ""),
            })
            continue
        data = json.loads(comparison_path.read_text(encoding="utf-8"))
        for item in data:
            rows.append({
                "variant": variant_dir.name,
                "variant_status": meta.get("status", "success"),
                "cache_name": str(item.get("cache_name", "")),
                "cache_status": str(item.get("status", "")),
                "mean_bfs_ms": as_float(item.get("mean_bfs_ms")),
                "mean_server_bfs_ms": as_float(item.get("mean_server_bfs_ms")),
                "a0_batch_size": meta.get("arango_traversal_batch_size", ""),
            })

    # Find the fastest successful A0 batch for each cache, then report A1 speedup against it.
    best_a0: dict[str, tuple[str, float]] = {}
    for row in rows:
        cache = str(row["cache_name"])
        variant = str(row["variant"])
        value = row["mean_server_bfs_ms"]
        if not cache or not variant.startswith("a0-") or value is None or row["cache_status"] != "success":
            continue
        current = best_a0.get(cache)
        if current is None or float(value) < current[1]:
            best_a0[cache] = (variant, float(value))

    for row in rows:
        cache = str(row["cache_name"])
        baseline = best_a0.get(cache)
        value = row["mean_server_bfs_ms"]
        row["best_a0_variant"] = baseline[0] if baseline else ""
        row["best_a0_server_bfs_ms"] = baseline[1] if baseline else None
        row["speedup_vs_best_a0"] = (
            baseline[1] / float(value)
            if baseline and value is not None and float(value) > 0
            else None
        )

    fields = [
        "variant", "variant_status", "cache_name", "cache_status", "a0_batch_size",
        "mean_bfs_ms", "mean_server_bfs_ms", "best_a0_variant", "best_a0_server_bfs_ms",
        "speedup_vs_best_a0",
    ]
    with (analysis / "statistics.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows({field: row.get(field, "") for field in fields} for row in rows)

    lines = [
        "# Arango A0/A1 benchmark summary",
        "",
        "A0 uses bounded graph-traversal batches; A1 uses direct native edge-index lookups.",
        "Speedup is computed from mean server BFS time against the fastest successful A0 batch for the same cache configuration.",
        "",
        "| Cache | Variant | Status | Server BFS mean (ms) | Best A0 | Speedup vs best A0 |",
        "|---|---|---:|---:|---|---:|",
    ]
    sortable = [r for r in rows if str(r.get("cache_name", ""))]
    sortable.sort(key=lambda r: (str(r["cache_name"]), str(r["variant"])))
    for row in sortable:
        server = row["mean_server_bfs_ms"]
        speedup = row["speedup_vs_best_a0"]
        lines.append(
            f"| {row['cache_name']} | {row['variant']} | {row['cache_status'] or row['variant_status']} | "
            f"{'n/a' if server is None else f'{float(server):.3f}'} | "
            f"{row['best_a0_variant'] or 'n/a'} | "
            f"{'n/a' if speedup is None else f'{float(speedup):.3f}x'} |"
        )

    failed = [r for r in rows if r["variant_status"] != "success"]
    if failed:
        lines += ["", "## Incomplete variants", ""]
        for variant in sorted({str(r["variant"]) for r in failed}):
            lines.append(f"- `{variant}` did not complete; later variants were still attempted.")

    (analysis / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(analysis / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
