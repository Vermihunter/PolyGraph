from __future__ import annotations

import csv
import json
from collections import defaultdict
from pathlib import Path
from statistics import mean, median
from typing import Any

import matplotlib.pyplot as plt


def _load_rows(measurements_csv: str | Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with open(measurements_csv, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["seconds"] = float(row["seconds"])
            row["repetition"] = int(row["repetition"])
            row["details"] = json.loads(row.get("details_json") or "{}")
            rows.append(row)
    return rows


def write_plots(measurements_csv: str | Path, output_dir: str | Path) -> list[Path]:
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = _load_rows(measurements_csv)
    outputs: list[Path] = []

    bfs_rows = [r for r in rows if r["phase"] == "bfs_query" and r["status"] == "ok"]
    if bfs_rows:
        grouped: dict[str, list[float]] = defaultdict(list)
        for row in bfs_rows:
            grouped[f"{row['scenario']}\n{row['system']}"].append(row["seconds"])
        labels = list(grouped)
        values = [grouped[label] for label in labels]

        plt.figure(figsize=(max(8, len(labels) * 1.1), 5))
        plt.boxplot(values, tick_labels=labels, showmeans=True)
        plt.ylabel("seconds")
        plt.title("BFS query runtime distribution")
        plt.xticks(rotation=25, ha="right")
        plt.tight_layout()
        path = out_dir / "bfs_query_runtime_boxplot.png"
        plt.savefig(path, dpi=160)
        plt.close()
        outputs.append(path)

        summary_path = out_dir / "bfs_query_summary.csv"
        with open(summary_path, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["scenario", "system", "runs", "mean_seconds", "median_seconds", "min_seconds", "max_seconds"])
            by_pair: dict[tuple[str, str], list[float]] = defaultdict(list)
            for row in bfs_rows:
                by_pair[(row["scenario"], row["system"])].append(row["seconds"])
            for (scenario, system), xs in sorted(by_pair.items()):
                writer.writerow([scenario, system, len(xs), mean(xs), median(xs), min(xs), max(xs)])
        outputs.append(summary_path)

    phase_rows = [r for r in rows if r["status"] == "ok" and r["phase"] in {"load", "bfs_query"}]
    if phase_rows:
        grouped_phase: dict[str, dict[str, float]] = defaultdict(lambda: defaultdict(float))
        for row in phase_rows:
            key = f"{row['scenario']}\n{row['system']}"
            grouped_phase[key][row["phase"]] += row["seconds"]
        labels = list(grouped_phase)
        load_values = [grouped_phase[label].get("load", 0.0) for label in labels]
        bfs_values = [grouped_phase[label].get("bfs_query", 0.0) for label in labels]
        x = range(len(labels))

        plt.figure(figsize=(max(8, len(labels) * 1.1), 5))
        plt.bar(x, load_values, label="load")
        plt.bar(x, bfs_values, bottom=load_values, label="bfs_query")
        plt.ylabel("total seconds")
        plt.title("Total measured time by phase")
        plt.xticks(list(x), labels, rotation=25, ha="right")
        plt.legend()
        plt.tight_layout()
        path = out_dir / "phase_time_breakdown.png"
        plt.savefig(path, dpi=160)
        plt.close()
        outputs.append(path)

    validation_rows = [r for r in rows if r["phase"] == "compare_reference" and r.get("same_as_reference") in {"False", "false", False}]
    if validation_rows:
        path = out_dir / "validation_failures.csv"
        with open(path, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=validation_rows[0].keys())
            writer.writeheader()
            writer.writerows(validation_rows)
        outputs.append(path)

    return outputs
