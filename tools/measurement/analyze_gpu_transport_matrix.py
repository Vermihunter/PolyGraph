#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.algorithms.bfs import Graph, load_ldbc_snb_graph, save_depths_to_csv
from tools.graphio.domain import load_domain_manifest
from tools.graphio.id_codec import parse_global_id
from tools.measurement.graph_bench.csv_compare import compare_bfs_csv


REP_PREFIX = re.compile(r"^r\d+-")
WARMUP_PREFIX = re.compile(r"^warmup-\d+-")


def percentile(values: list[float], q: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * q
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (pos - lo)


def stats(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {"n": 0, "mean": math.nan, "median": math.nan, "stdev": math.nan,
                "p95": math.nan, "min": math.nan, "max": math.nan}
    return {
        "n": len(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "p95": percentile(values, 0.95),
        "min": min(values),
        "max": max(values),
    }


def category_from_request_id(request_id: str) -> str:
    value = REP_PREFIX.sub("", request_id)
    value = WARMUP_PREFIX.sub("", value)
    return value


def read_measurements(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle, delimiter=";"))


def find_run_dirs(variant_dir: Path) -> list[Path]:
    """Return every cache-plan run contained in a GPU variant directory."""
    direct = variant_dir / "measurements.csv"
    if direct.is_file():
        return [variant_dir]
    return sorted(
        p for p in variant_dir.iterdir()
        if p.is_dir() and (p / "measurements.csv").is_file()
    )


def write_distance_csv(distances: dict[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = [(str(k), int(v)) for k, v in distances.items()]
    rows.sort(key=lambda item: (item[1], item[0]))
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter=";")
        writer.writerow(["vertex_id", "distance"])
        writer.writerows(rows)


def discover_ldbc_csv_snapshot(domain_path: Path) -> Path:
    """Find the original extracted CsvBasic snapshot belonging to a prepared domain."""
    scale_root = domain_path.parent.parent
    raw_root = scale_root / "raw"
    if not raw_root.is_dir():
        raise FileNotFoundError(f"LDBC raw-data directory not found: {raw_root}")

    candidates: list[Path] = []
    if (raw_root / "static").is_dir() and (raw_root / "dynamic").is_dir():
        candidates.append(raw_root)
    for child in sorted(raw_root.iterdir()):
        if child.is_dir() and (child / "static").is_dir() and (child / "dynamic").is_dir():
            candidates.append(child)

    if not candidates:
        raise FileNotFoundError(
            f"No extracted LDBC CsvBasic snapshot with static/ and dynamic/ found under {raw_root}"
        )
    if len(candidates) != 1:
        rendered = ", ".join(str(path) for path in candidates)
        raise ValueError(f"Multiple LDBC CsvBasic snapshots found under {raw_root}: {rendered}")
    return candidates[0].resolve()


def ldbc_source_metadata(
    input_dir: Path, schema: Path, native_id_map: Path | None
) -> dict[str, Any]:
    """Cheap cache fingerprint: paths + sizes + mtimes, never graph.jsonl contents."""
    files = []
    for path in sorted(input_dir.rglob("*.csv")):
        if "updatestream" in path.name.lower() or "update_stream" in path.name.lower():
            continue
        st = path.stat()
        files.append({
            "path": str(path.relative_to(input_dir)),
            "size": st.st_size,
            "mtimeNs": st.st_mtime_ns,
        })
    if not files:
        raise FileNotFoundError(f"No LDBC CSV files found under {input_dir}")

    schema_stat = schema.stat()
    meta: dict[str, Any] = {
        "sourceKind": "ldbc-snb-csv-v1",
        "inputDir": str(input_dir),
        "schema": str(schema),
        "schemaSize": schema_stat.st_size,
        "schemaMtimeNs": schema_stat.st_mtime_ns,
        "csvFiles": files,
    }
    if native_id_map is not None:
        mapping_stat = native_id_map.stat()
        meta.update({
            "nativeIdMap": str(native_id_map),
            "nativeIdMapSize": mapping_stat.st_size,
            "nativeIdMapMtimeNs": mapping_stat.st_mtime_ns,
        })
    return meta


class LdbcCsvReferenceCache:
    """Lazy, cached Python BFS references built from the original SNB CSVs.

    The raw dataset is parsed at most once per traversal mode in one analysis
    process. Individual BFS outputs are persisted under _reference and reused by
    every GPU variant and by later analyzer invocations.
    """

    def __init__(
        self,
        *,
        input_dir: Path,
        schema: Path,
        native_id_map: Path | None,
        cache_dir: Path,
    ) -> None:
        self.input_dir = input_dir.resolve()
        self.schema = schema.resolve()
        self.native_id_map = native_id_map.resolve() if native_id_map is not None else None
        self.cache_dir = cache_dir.resolve()
        self.source_meta = ldbc_source_metadata(
            self.input_dir, self.schema, self.native_id_map
        )
        self.source_digest = hashlib.sha256(
            json.dumps(self.source_meta, sort_keys=True).encode("utf-8")
        ).hexdigest()
        self._graphs: dict[bool, Graph] = {}

    def _graph(self, *, directed: bool) -> Graph:
        graph = self._graphs.get(directed)
        if graph is None:
            mode = "directed" if directed else "undirected/both"
            print(
                f"[gpu-analysis] loading raw LDBC CSV reference graph once ({mode}): {self.input_dir}",
                flush=True,
            )
            graph = load_ldbc_snb_graph(
                self.input_dir,
                schema_path=self.schema,
                native_id_map_path=self.native_id_map,
                undirected=not directed,
            )
            self._graphs[directed] = graph
        return graph

    def ensure(self, *, start_uuid: str, max_depth: int, direction: str) -> tuple[Path, bool]:
        directed = direction == "directed"
        meta = {
            "sourceKind": "ldbc-snb-csv-v1",
            "sourceDigest": self.source_digest,
            "startUuid": str(start_uuid),
            "maxDepth": int(max_depth),
            "directed": bool(directed),
        }
        digest = hashlib.sha256(json.dumps(meta, sort_keys=True).encode("utf-8")).hexdigest()[:20]
        out = self.cache_dir / f"reference-{digest}.csv"
        meta_path = self.cache_dir / f"reference-{digest}.json"
        if out.is_file() and meta_path.is_file():
            return out, False

        self.cache_dir.mkdir(parents=True, exist_ok=True)
        print(
            f"[gpu-analysis] reference BFS from raw CSV: start={start_uuid} depth={max_depth} direction={direction}",
            flush=True,
        )
        graph = self._graph(directed=directed)
        depth_limit = None if max_depth < 0 else max_depth
        depths = graph.bfs_depths(parse_global_id(start_uuid), max_depth=depth_limit)
        save_depths_to_csv(depths, out)
        meta_path.write_text(
            json.dumps({**meta, "source": self.source_meta}, indent=2) + "\n",
            encoding="utf-8",
        )
        return out, True


def correctness_for_variant(*, variant: str, run_dir: Path,
                            reference_cache: LdbcCsvReferenceCache,
                            correctness_dir: Path) -> tuple[list[dict[str, Any]], int, int]:
    rows: list[dict[str, Any]] = []
    generated = reused = 0
    request_dir = run_dir / "requests"
    output_dir = run_dir / "outputs" / "requests"
    if not request_dir.is_dir() or not output_dir.is_dir():
        return rows, generated, reused

    for request_path in sorted(request_dir.glob("warmup-*.json")):
        request = json.loads(request_path.read_text(encoding="utf-8"))
        request_id = str(request.get("id") or request_path.stem)
        category = category_from_request_id(request_id)
        output_path = output_dir / f"{request_id}.json"
        if not output_path.is_file():
            rows.append({"variant": variant, "category": category, "status": "MISSING_OUTPUT"})
            continue
        response = json.loads(output_path.read_text(encoding="utf-8"))
        distances = ((response.get("result") or {}).get("distance"))
        if not isinstance(distances, dict):
            rows.append({
                "variant": variant,
                "category": category,
                "status": "NO_FULL_RESULT",
                "detail": "warm-up response has no result.distance; rerun benchmark after applying this patch",
            })
            continue

        payload = request.get("payload") or {}
        max_depth = int(payload.get("maxDepth", response.get("maxDepth", 7)))
        direction = str(payload.get("direction", "both")).lower()
        start_uuid = str(response.get("sourceVertexId") or request.get("startUuid"))
        expected, created = reference_cache.ensure(
            start_uuid=start_uuid,
            max_depth=max_depth,
            direction=direction,
        )
        generated += int(created)
        reused += int(not created)

        safe = re.sub(r"[^A-Za-z0-9._-]+", "-", category)
        actual = correctness_dir / variant / f"{safe}.actual.csv"
        diff = correctness_dir / variant / f"{safe}.diff.csv"
        write_distance_csv(distances, actual)
        cmp = compare_bfs_csv(expected, actual, diff)
        rows.append({
            "variant": variant,
            "category": category,
            "start_uuid": start_uuid,
            "max_depth": max_depth,
            "direction": direction,
            "status": "PASS" if cmp.same else "FAIL",
            "expected_count": cmp.expected_count,
            "actual_count": cmp.actual_count,
            "missing_count": cmp.missing_count,
            "extra_count": cmp.extra_count,
            "wrong_distance_count": cmp.wrong_distance_count,
            "reference_csv": str(expected),
            "actual_csv": str(actual),
            "diff_csv": cmp.diff_csv,
        })
    return rows, generated, reused


def write_csv(path: Path, rows: list[dict[str, Any]], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def fmt(value: float) -> str:
    return "n/a" if math.isnan(value) else f"{value:.3f}"


def make_plots(analysis_dir: Path, timings: dict[str, list[dict[str, Any]]], baseline: str) -> list[Path]:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("[gpu-analysis] warning: matplotlib is unavailable; skipping plots", file=sys.stderr)
        return []

    plots: list[Path] = []
    variants = list(timings)
    data = [[float(r["server_bfs_ms"]) for r in timings[v]] for v in variants]
    if variants and any(data):
        width = max(10.0, len(variants) * 1.25)
        fig, ax = plt.subplots(figsize=(width, 6.2))
        ax.boxplot(data, tick_labels=variants, showfliers=True)
        ax.set_ylabel("Server BFS time (ms)")
        ax.set_title("GPU execution variants — BFS latency distribution")
        ax.tick_params(axis="x", rotation=35)
        ax.grid(axis="y", alpha=0.25)
        fig.tight_layout()
        path = analysis_dir / "server_bfs_boxplot.png"
        fig.savefig(path, dpi=180)
        plt.close(fig)
        plots.append(path)

    medians = {v: statistics.median([float(r["server_bfs_ms"]) for r in rows]) for v, rows in timings.items() if rows}
    speedups: list[tuple[str, float]] = []
    for variant, median in medians.items():
        matched_baseline = baseline
        if "/" in variant and f"{baseline}/{variant.split('/', 1)[1]}" in medians:
            matched_baseline = f"{baseline}/{variant.split('/', 1)[1]}"
        if matched_baseline not in medians:
            continue
        b = medians[matched_baseline]
        speedups.append((variant, b / median if median > 0 else math.nan))
    if speedups:
        fig, ax = plt.subplots(figsize=(max(10.0, len(speedups) * 1.15), 6.0))
        ax.bar([v for v, _ in speedups], [value for _, value in speedups])
        ax.axhline(1.0, linewidth=1.0)
        ax.set_ylabel(f"Median speedup vs matched {baseline}")
        ax.set_title("GPU execution speedup")
        ax.tick_params(axis="x", rotation=35)
        ax.grid(axis="y", alpha=0.25)
        fig.tight_layout()
        path = analysis_dir / "speedup_vs_baseline.png"
        fig.savefig(path, dpi=180)
        plt.close(fig)
        plots.append(path)

    categories = sorted({str(r["category"]) for rows in timings.values() for r in rows})
    if categories and variants:
        fig, ax = plt.subplots(figsize=(max(11.0, len(categories) * 2.5), 6.5))
        x = list(range(len(categories)))
        total_width = 0.82
        bar_width = total_width / max(1, len(variants))
        for i, variant in enumerate(variants):
            by_cat: dict[str, list[float]] = defaultdict(list)
            for row in timings[variant]:
                by_cat[str(row["category"])].append(float(row["server_bfs_ms"]))
            means = [statistics.fmean(by_cat[c]) if by_cat[c] else math.nan for c in categories]
            positions = [value - total_width / 2 + bar_width / 2 + i * bar_width for value in x]
            ax.bar(positions, means, width=bar_width, label=variant)
        ax.set_xticks(x, categories, rotation=20)
        ax.set_ylabel("Mean server BFS time (ms)")
        ax.set_title("GPU execution performance by request category")
        ax.grid(axis="y", alpha=0.25)
        ax.legend(fontsize="small", ncols=2)
        fig.tight_layout()
        path = analysis_dir / "server_bfs_by_category.png"
        fig.savefig(path, dpi=180)
        plt.close(fig)
        plots.append(path)
    return plots


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze a completed GPU transport benchmark matrix.")
    parser.add_argument("--root", required=True, help="GPU matrix output directory")
    parser.add_argument("--config", required=True, help="GPU benchmark JSON configuration")
    parser.add_argument("--baseline", default="v0-baseline")
    args = parser.parse_args()

    matrix_root = Path(args.root).expanduser().resolve()
    cfg_path = Path(args.config).expanduser().resolve()
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    domain_path = Path(str(cfg["domain"]))
    if not domain_path.is_absolute():
        domain_path = (ROOT / domain_path).resolve()
    domain = load_domain_manifest(domain_path)
    schema = domain.paths.schema
    if not schema.is_file():
        raise FileNotFoundError(f"Reference graph schema not found: {schema}")
    ldbc_input = discover_ldbc_csv_snapshot(domain_path)

    analysis_dir = matrix_root / "_analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)
    reference_dir = matrix_root / "_reference"
    correctness_dir = analysis_dir / "correctness"
    native_id_map = domain.paths.native_id_map
    if native_id_map is not None and not native_id_map.is_file():
        raise FileNotFoundError(f"Native ID map referenced by domain does not exist: {native_id_map}")
    reference_cache = LdbcCsvReferenceCache(
        input_dir=ldbc_input,
        schema=schema,
        native_id_map=native_id_map,
        cache_dir=reference_dir,
    )

    # benchmark_compact_repeated.py --overwrite recreates each variant output
    # directory. Older matrix runs therefore may not contain variant.env even
    # though all benchmark results are valid. Discover variants by their actual
    # measurement output; variant.env is metadata, not a validity marker.
    variant_dirs = sorted(
        p
        for p in matrix_root.iterdir()
        if p.is_dir()
        and p.name not in {"_analysis", "_reference"}
        and bool(find_run_dirs(p))
    )
    if not variant_dirs:
        raise ValueError(f"No GPU variant outputs with measurements.csv found under {matrix_root}")

    timings: dict[str, list[dict[str, Any]]] = {}
    stat_rows: list[dict[str, Any]] = []
    correctness_rows: list[dict[str, Any]] = []
    references_generated = references_reused = 0

    for variant_dir in variant_dirs:
        variant = variant_dir.name
        run_dirs = find_run_dirs(variant_dir)
        if not run_dirs:
            print(f"[gpu-analysis] warning: no measurements.csv for {variant}", file=sys.stderr)
            continue
        multi_cache = len(run_dirs) > 1

        for run_dir in run_dirs:
            cache_name = run_dir.name if run_dir != variant_dir else "direct"
            variant_label = f"{variant}/{cache_name}" if multi_cache else variant
            measured_rows: list[dict[str, Any]] = []
            for row in read_measurements(run_dir / "measurements.csv"):
                measured = str(row.get("measured", "true")).lower() not in {"false", "0", "no"}
                if not measured or row.get("status") != "success" or not row.get("server_bfs_ms"):
                    continue
                measured_rows.append({
                    **row,
                    "cache_name": cache_name,
                    "category": category_from_request_id(str(row.get("request_id", ""))),
                })
            timings[variant_label] = measured_rows

            overall = stats([float(r["server_bfs_ms"]) for r in measured_rows])
            stat_rows.append({"variant": variant_label, "category": "ALL", **overall})
            categories = sorted({str(r["category"]) for r in measured_rows})
            for category in categories:
                values = [float(r["server_bfs_ms"]) for r in measured_rows if r["category"] == category]
                stat_rows.append({"variant": variant_label, "category": category, **stats(values)})

            rows, created, reused = correctness_for_variant(
                variant=variant_label,
                run_dir=run_dir,
                reference_cache=reference_cache,
                correctness_dir=correctness_dir,
            )
            correctness_rows.extend(rows)
            references_generated += created
            references_reused += reused

    overall_by_variant = {r["variant"]: r for r in stat_rows if r["category"] == "ALL"}
    for row in stat_rows:
        if row["category"] != "ALL":
            row["median_speedup_vs_baseline"] = ""
            continue
        variant = str(row["variant"])
        matched_baseline = args.baseline
        if "/" in variant:
            cache_suffix = variant.split("/", 1)[1]
            candidate = f"{args.baseline}/{cache_suffix}"
            if candidate in overall_by_variant:
                matched_baseline = candidate
        baseline_row = overall_by_variant.get(matched_baseline)
        if baseline_row is None:
            row["median_speedup_vs_baseline"] = ""
            continue
        baseline_median = float(baseline_row.get("median", math.nan))
        median = float(row["median"])
        row["median_speedup_vs_baseline"] = (
            baseline_median / median
            if median > 0 and not math.isnan(baseline_median)
            else math.nan
        )

    write_csv(
        analysis_dir / "statistics.csv",
        stat_rows,
        ["variant", "category", "n", "mean", "median", "stdev", "p95", "min", "max", "median_speedup_vs_baseline"],
    )
    correctness_fields = [
        "variant", "category", "start_uuid", "max_depth", "direction", "status",
        "expected_count", "actual_count", "missing_count", "extra_count", "wrong_distance_count",
        "reference_csv", "actual_csv", "diff_csv", "detail",
    ]
    write_csv(analysis_dir / "correctness.csv", correctness_rows, correctness_fields)
    plots = make_plots(analysis_dir, timings, args.baseline)

    lines = [
        "PolyGraph GPU transport benchmark analysis",
        "=========================================",
        f"matrix: {matrix_root}",
        f"reference CSVs: {ldbc_input}",
        f"reference schema: {schema}",
        f"native ID map: {native_id_map or 'none'}",
        f"baseline: {args.baseline}",
        "",
        "Performance (measured requests only)",
        "------------------------------------",
        f"{'variant':28} {'n':>5} {'mean ms':>12} {'median ms':>12} {'p95 ms':>12} {'stdev':>12} {'speedup':>10}",
    ]
    for variant in timings:
        row = overall_by_variant.get(variant)
        if not row:
            continue
        speed = row.get("median_speedup_vs_baseline", "")
        speed_text = f"{float(speed):.3f}x" if speed != "" and not math.isnan(float(speed)) else "n/a"
        lines.append(
            f"{variant:28} {int(row['n']):5d} {fmt(float(row['mean'])):>12} {fmt(float(row['median'])):>12} "
            f"{fmt(float(row['p95'])):>12} {fmt(float(row['stdev'])):>12} {speed_text:>10}"
        )

    lines += ["", "Correctness vs cached Python BFS reference (original LDBC CSVs)", "------------------------------------------"]
    if correctness_rows:
        for row in correctness_rows:
            detail = ""
            if row.get("status") == "FAIL":
                detail = (f" missing={row.get('missing_count')} extra={row.get('extra_count')} "
                          f"wrong_distance={row.get('wrong_distance_count')}")
            elif row.get("status") not in {"PASS", "FAIL"}:
                detail = " " + str(row.get("detail", ""))
            lines.append(f"{row['variant']:28} {row['category']:32} {row['status']}{detail}")
    else:
        lines.append("No warm-up correctness outputs were found.")
    lines += [
        "",
        f"Reference cache: generated {references_generated}, reused {references_reused}",
        f"Statistics CSV: {analysis_dir / 'statistics.csv'}",
        f"Correctness CSV: {analysis_dir / 'correctness.csv'}",
    ]
    for plot in plots:
        lines.append(f"Plot: {plot}")

    report = analysis_dir / "summary.txt"
    report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))

    failures = [r for r in correctness_rows if r.get("status") not in {"PASS"}]
    if failures:
        print(f"[gpu-analysis] correctness failures/incomplete checks: {len(failures)}", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
