#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable


def load_env(path: Path) -> dict[str, str]:
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


def as_bool(value: object) -> bool:
    return str(value).strip().lower() in {"1", "true", "yes", "on"}


def as_float(value: object) -> float | None:
    try:
        if value is None or str(value).strip() == "":
            return None
        return float(value)
    except (TypeError, ValueError):
        return None


def percentile(values: list[float], p: float) -> float:
    if not values:
        raise ValueError("percentile requires values")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * p
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]
    fraction = rank - low
    return ordered[low] * (1.0 - fraction) + ordered[high] * fraction


def describe(values: Iterable[float]) -> dict[str, float | int | None]:
    xs = list(values)
    if not xs:
        return {
            "n": 0,
            "mean": None,
            "median": None,
            "p95": None,
            "stdev": None,
            "cv_pct": None,
            "min": None,
            "max": None,
        }
    mean = statistics.fmean(xs)
    stdev = statistics.stdev(xs) if len(xs) >= 2 else 0.0
    return {
        "n": len(xs),
        "mean": mean,
        "median": statistics.median(xs),
        "p95": percentile(xs, 0.95),
        "stdev": stdev,
        "cv_pct": (stdev / mean * 100.0) if mean else None,
        "min": min(xs),
        "max": max(xs),
    }


def fmt_ms(value: object) -> str:
    number = as_float(value)
    return "n/a" if number is None else f"{number:,.2f}"


def fmt_pct(value: object) -> str:
    number = as_float(value)
    return "n/a" if number is None else f"{number:.2f}%"


def fmt_x(value: object) -> str:
    number = as_float(value)
    return "n/a" if number is None else f"{number:.3f}x"


def variant_label(variant: str) -> str:
    return {
        "a1-edge-index": "A1 edge-index",
        "a1-edge-index-plan-cache": "A1 + plan cache",
    }.get(variant, variant)


def discover(root: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    measured: list[dict[str, Any]] = []
    cold: list[dict[str, Any]] = []
    passes: list[dict[str, Any]] = []

    for pass_dir in sorted(p for p in root.glob("pass-*") if p.is_dir()):
        variant_dirs = [p for p in pass_dir.iterdir() if p.is_dir() and not p.name.startswith("_")]
        for variant_dir in sorted(variant_dirs):
            env = load_env(variant_dir / "arango-access.env")
            variant = env.get("variant", variant_dir.name)
            reverse = as_bool(env.get("reverse_cache_order", "0"))
            pass_status = env.get("status", "unknown")
            passes.append(
                {
                    "pass": pass_dir.name,
                    "variant": variant,
                    "reverse_cache_order": reverse,
                    "status": pass_status,
                }
            )

            for cache_dir in sorted(p for p in variant_dir.iterdir() if p.is_dir() and not p.name.startswith("_")):
                measurements = cache_dir / "measurements.csv"
                if measurements.is_file():
                    with measurements.open(newline="", encoding="utf-8") as handle:
                        for row in csv.DictReader(handle, delimiter=";"):
                            if not as_bool(row.get("measured")) or row.get("status") != "success":
                                continue
                            server = as_float(row.get("server_bfs_ms"))
                            http = as_float(row.get("duration_ms"))
                            if server is None or http is None:
                                continue
                            measured.append(
                                {
                                    "pass": pass_dir.name,
                                    "variant": variant,
                                    "cache_name": cache_dir.name,
                                    "reverse_cache_order": reverse,
                                    "request_id": row.get("request_id", ""),
                                    "start_uuid": row.get("start_uuid", ""),
                                    "repetition": int(row["repetition"]) if row.get("repetition") else None,
                                    "server_bfs_ms": server,
                                    "http_ms": http,
                                }
                            )

                prime = cache_dir / "cold-prime.json"
                if prime.is_file():
                    item = json.loads(prime.read_text(encoding="utf-8"))
                    cold.append(
                        {
                            "pass": pass_dir.name,
                            "variant": variant,
                            "cache_name": cache_dir.name,
                            "reverse_cache_order": reverse,
                            "status": item.get("status", ""),
                            "start_uuid": item.get("startUuid", ""),
                            "http_ms": as_float(item.get("durationMs")),
                            "server_bfs_ms": as_float(item.get("serverBfsMs")),
                            "timeout_seconds": as_float(item.get("timeoutSeconds")),
                            "error": item.get("error", ""),
                        }
                    )
    return measured, cold, passes


def write_csv(path: Path, rows: list[dict[str, Any]], fields: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows({field: row.get(field, "") for field in fields} for row in rows)


def aggregate(measured: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in measured:
        grouped[(row["variant"], row["cache_name"])].append(row)

    result: list[dict[str, Any]] = []
    for (variant, cache), rows in sorted(grouped.items()):
        server = describe(float(r["server_bfs_ms"]) for r in rows)
        http = describe(float(r["http_ms"]) for r in rows)
        result.append(
            {
                "variant": variant,
                "cache_name": cache,
                "passes": len({str(r["pass"]) for r in rows}),
                "measured_bfs_n": server["n"],
                "server_mean_ms": server["mean"],
                "server_median_ms": server["median"],
                "server_p95_ms": server["p95"],
                "server_stdev_ms": server["stdev"],
                "server_cv_pct": server["cv_pct"],
                "server_min_ms": server["min"],
                "server_max_ms": server["max"],
                "http_mean_ms": http["mean"],
                "http_median_ms": http["median"],
                "http_p95_ms": http["p95"],
            }
        )
    return result


def aggregate_per_root(measured: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in measured:
        grouped[(row["variant"], row["cache_name"], row["start_uuid"])].append(row)

    result: list[dict[str, Any]] = []
    for (variant, cache, root), rows in sorted(grouped.items()):
        server = describe(float(r["server_bfs_ms"]) for r in rows)
        result.append(
            {
                "variant": variant,
                "cache_name": cache,
                "start_uuid": root,
                "n": server["n"],
                "mean_ms": server["mean"],
                "median_ms": server["median"],
                "p95_ms": server["p95"],
                "stdev_ms": server["stdev"],
                "cv_pct": server["cv_pct"],
                "min_ms": server["min"],
                "max_ms": server["max"],
            }
        )
    return result


def aggregate_cold(cold: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in cold:
        if row.get("status") == "success" and row.get("server_bfs_ms") is not None:
            grouped[(row["variant"], row["cache_name"])].append(row)

    result: list[dict[str, Any]] = []
    for (variant, cache), rows in sorted(grouped.items()):
        server = describe(float(r["server_bfs_ms"]) for r in rows)
        http = describe(float(r["http_ms"]) for r in rows if r.get("http_ms") is not None)
        result.append(
            {
                "variant": variant,
                "cache_name": cache,
                "n": server["n"],
                "server_mean_ms": server["mean"],
                "server_median_ms": server["median"],
                "server_min_ms": server["min"],
                "server_max_ms": server["max"],
                "http_mean_ms": http["mean"],
            }
        )
    return result


def pairwise(stats: list[dict[str, Any]]) -> list[dict[str, Any]]:
    lookup = {(str(r["variant"]), str(r["cache_name"])): r for r in stats}
    caches = sorted({str(r["cache_name"]) for r in stats})
    result: list[dict[str, Any]] = []

    baseline_variant = "a1-edge-index"
    plan_variant = "a1-edge-index-plan-cache"
    for cache in caches:
        base = lookup.get((baseline_variant, cache))
        plan = lookup.get((plan_variant, cache))
        if base and plan:
            b = as_float(base.get("server_mean_ms"))
            p = as_float(plan.get("server_mean_ms"))
            if b and p:
                result.append(
                    {
                        "comparison": "plan-cache",
                        "cache_name": cache,
                        "variant": plan_variant,
                        "baseline": baseline_variant,
                        "baseline_mean_ms": b,
                        "candidate_mean_ms": p,
                        "speedup": b / p,
                        "reduction_pct": (1.0 - p / b) * 100.0,
                    }
                )

    no_cache_candidates = sorted(c for c in caches if "no-cache" in c)
    if no_cache_candidates:
        no_cache = no_cache_candidates[0]
        for variant in sorted({str(r["variant"]) for r in stats}):
            baseline = lookup.get((variant, no_cache))
            baseline_mean = as_float(baseline.get("server_mean_ms")) if baseline else None
            if not baseline_mean:
                continue
            for cache in caches:
                if cache == no_cache:
                    continue
                candidate = lookup.get((variant, cache))
                candidate_mean = as_float(candidate.get("server_mean_ms")) if candidate else None
                if not candidate_mean:
                    continue
                result.append(
                    {
                        "comparison": "polygraph-cache-vs-no-cache",
                        "cache_name": cache,
                        "variant": variant,
                        "baseline": no_cache,
                        "baseline_mean_ms": baseline_mean,
                        "candidate_mean_ms": candidate_mean,
                        "speedup": baseline_mean / candidate_mean,
                        "reduction_pct": (1.0 - candidate_mean / baseline_mean) * 100.0,
                    }
                )
    return result


def create_plots(analysis: Path, measured: list[dict[str, Any]], stats: list[dict[str, Any]], pairs: list[dict[str, Any]]) -> list[str]:
    created: list[str] = []
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # noqa: BLE001
        print(f"[arango-overnight-analysis] plots skipped: {exc}")
        return created

    variants = ["a1-edge-index", "a1-edge-index-plan-cache"]
    caches = sorted({str(r["cache_name"]) for r in measured})
    data: list[list[float]] = []
    labels: list[str] = []
    for cache in caches:
        for variant in variants:
            values = [
                float(r["server_bfs_ms"])
                for r in measured
                if r["cache_name"] == cache and r["variant"] == variant
            ]
            if values:
                data.append(values)
                short_variant = "A1" if variant == "a1-edge-index" else "A1+PC"
                labels.append(f"{cache}\n{short_variant}")
    if data:
        fig, ax = plt.subplots(figsize=(12, 6))
        ax.boxplot(data, showfliers=True)
        ax.set_xticks(range(1, len(labels) + 1), labels)
        ax.set_yscale("log")
        ax.set_ylabel("Server BFS time (ms, log scale)")
        ax.set_title("Arango A1 warm measured BFS distributions")
        ax.tick_params(axis="x", labelrotation=20)
        ax.grid(axis="y", alpha=0.25)
        fig.tight_layout()
        path = analysis / "server_bfs_boxplot.png"
        fig.savefig(path, dpi=180)
        plt.close(fig)
        created.append(path.name)

    plan_pairs = [p for p in pairs if p["comparison"] == "plan-cache"]
    if plan_pairs:
        fig, ax = plt.subplots(figsize=(9, 5))
        labels = [str(p["cache_name"]) for p in plan_pairs]
        values = [float(p["speedup"]) for p in plan_pairs]
        ax.bar(labels, values)
        ax.axhline(1.0, linewidth=1)
        ax.set_ylabel("Speedup (A1 / A1+plan-cache)")
        ax.set_title("AQL plan-cache effect")
        ax.tick_params(axis="x", labelrotation=20)
        ax.grid(axis="y", alpha=0.25)
        fig.tight_layout()
        path = analysis / "plan_cache_speedup.png"
        fig.savefig(path, dpi=180)
        plt.close(fig)
        created.append(path.name)

    return created


def main() -> int:
    parser = argparse.ArgumentParser(description="Merge and summarize the PolyGraph Arango overnight benchmark.")
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    root = Path(args.root).expanduser().resolve()
    analysis = root / "_analysis"
    analysis.mkdir(parents=True, exist_ok=True)

    measured, cold, passes = discover(root)
    if not measured:
        raise RuntimeError(f"No successful measured BFS rows were found under {root}")

    stats = aggregate(measured)
    per_root = aggregate_per_root(measured)
    cold_stats = aggregate_cold(cold)
    pairs = pairwise(stats)

    stats_fields = [
        "variant", "cache_name", "passes", "measured_bfs_n",
        "server_mean_ms", "server_median_ms", "server_p95_ms", "server_stdev_ms",
        "server_cv_pct", "server_min_ms", "server_max_ms",
        "http_mean_ms", "http_median_ms", "http_p95_ms",
    ]
    write_csv(analysis / "statistics.csv", stats, stats_fields)
    write_csv(
        analysis / "per_root.csv",
        per_root,
        ["variant", "cache_name", "start_uuid", "n", "mean_ms", "median_ms", "p95_ms", "stdev_ms", "cv_pct", "min_ms", "max_ms"],
    )
    write_csv(
        analysis / "cold_prime.csv",
        cold,
        ["pass", "variant", "cache_name", "reverse_cache_order", "status", "start_uuid", "http_ms", "server_bfs_ms", "timeout_seconds", "error"],
    )
    write_csv(
        analysis / "cold_prime_statistics.csv",
        cold_stats,
        ["variant", "cache_name", "n", "server_mean_ms", "server_median_ms", "server_min_ms", "server_max_ms", "http_mean_ms"],
    )
    write_csv(
        analysis / "pairwise.csv",
        pairs,
        ["comparison", "cache_name", "variant", "baseline", "baseline_mean_ms", "candidate_mean_ms", "speedup", "reduction_pct"],
    )
    write_csv(
        analysis / "passes.csv",
        passes,
        ["pass", "variant", "reverse_cache_order", "status"],
    )

    plots = create_plots(analysis, measured, stats, pairs)

    suite_env = load_env(root / "overnight.env")
    lines = [
        "# PolyGraph Arango overnight benchmark",
        "",
        "## Experiment design",
        "",
        f"- Repetitions per pass: **{suite_env.get('repetitions_per_pass', 'unknown')}**.",
        f"- Total measured rounds per variant/cache: **{suite_env.get('measured_rounds_per_variant_cache', 'unknown')}**.",
        "- Two passes per A1 variant: one forward and one reverse cache order.",
        "- Variant order is balanced across the run (A1, A1+plan-cache, A1+plan-cache, A1).",
        "- ArangoDB is process-cold restarted before every cache configuration; persistent graph volumes are preserved.",
        "- One cold-prime BFS is recorded but excluded from warm measured statistics.",
        "- Linux host page cache is deliberately preserved so restarting Arango does not simultaneously cold-start MMAP/Neo4j/PostgreSQL.",
        "- A0 full-BFS traversal is intentionally excluded from the overnight repetition budget because prior bounded A0 variants did not complete their cold first BFS within the experimental deadlines.",
        "",
        "## Warm measured BFS",
        "",
        "| Cache | Variant | n | Mean server (ms) | Median (ms) | p95 (ms) | CV |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    for row in sorted(stats, key=lambda r: (str(r["cache_name"]), str(r["variant"]))):
        lines.append(
            f"| {row['cache_name']} | {variant_label(str(row['variant']))} | {row['measured_bfs_n']} | "
            f"{fmt_ms(row['server_mean_ms'])} | {fmt_ms(row['server_median_ms'])} | "
            f"{fmt_ms(row['server_p95_ms'])} | {fmt_pct(row['server_cv_pct'])} |"
        )

    plan_pairs = [p for p in pairs if p["comparison"] == "plan-cache"]
    if plan_pairs:
        lines += [
            "",
            "## AQL plan-cache effect",
            "",
            "Speedup is the aggregate warm mean of plain A1 divided by A1+plan-cache for the same cache configuration.",
            "",
            "| Cache | A1 mean (ms) | A1+PC mean (ms) | Speedup | Time reduction |",
            "|---|---:|---:|---:|---:|",
        ]
        for row in plan_pairs:
            lines.append(
                f"| {row['cache_name']} | {fmt_ms(row['baseline_mean_ms'])} | {fmt_ms(row['candidate_mean_ms'])} | "
                f"{fmt_x(row['speedup'])} | {fmt_pct(row['reduction_pct'])} |"
            )

    cache_pairs = [p for p in pairs if p["comparison"] == "polygraph-cache-vs-no-cache"]
    if cache_pairs:
        lines += [
            "",
            "## PolyGraph cache-placement effect",
            "",
            "Speedup is relative to the no-cache configuration under the same Arango access variant.",
            "",
            "| Variant | Cache | No-cache mean (ms) | Candidate mean (ms) | Speedup |",
            "|---|---|---:|---:|---:|",
        ]
        for row in cache_pairs:
            lines.append(
                f"| {variant_label(str(row['variant']))} | {row['cache_name']} | "
                f"{fmt_ms(row['baseline_mean_ms'])} | {fmt_ms(row['candidate_mean_ms'])} | {fmt_x(row['speedup'])} |"
            )

    if cold_stats:
        lines += [
            "",
            "## Cold-prime BFS",
            "",
            "These samples are diagnostic only and are not included in warm performance aggregates.",
            "",
            "| Cache | Variant | n | Cold server mean (ms) | Median (ms) |",
            "|---|---|---:|---:|---:|",
        ]
        for row in sorted(cold_stats, key=lambda r: (str(r["cache_name"]), str(r["variant"]))):
            lines.append(
                f"| {row['cache_name']} | {variant_label(str(row['variant']))} | {row['n']} | "
                f"{fmt_ms(row['server_mean_ms'])} | {fmt_ms(row['server_median_ms'])} |"
            )

    failed = [p for p in passes if p["status"] != "success"]
    if failed:
        lines += ["", "## Incomplete passes", ""]
        for item in failed:
            lines.append(f"- `{item['pass']}` ({item['variant']}) status: `{item['status']}`")

    if plots:
        lines += ["", "## Generated figures", ""]
        for name in plots:
            lines.append(f"- `{name}`")

    lines += [
        "",
        "## Files",
        "",
        "- `statistics.csv`: aggregate warm measurements by variant/cache.",
        "- `per_root.csv`: warm statistics separately for every BFS root.",
        "- `cold_prime.csv`: every cold-prime request.",
        "- `pairwise.csv`: plan-cache and cache-vs-no-cache speedups.",
        "- `passes.csv`: pass order/status metadata.",
    ]

    (analysis / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (analysis / "summary.json").write_text(
        json.dumps(
            {
                "statistics": stats,
                "perRoot": per_root,
                "coldPrimeStatistics": cold_stats,
                "pairwise": pairs,
                "passes": passes,
                "plots": plots,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(analysis / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
