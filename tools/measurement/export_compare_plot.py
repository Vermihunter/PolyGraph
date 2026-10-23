#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

THIS_FILE = Path(__file__).resolve()
REPO_ROOT = THIS_FILE.parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.measurement.compare_only import FAMILY_LABELS, load_samples

PREFERRED_METRICS = (
    "event:full_bfs",
    "family:full_bfs",
    "event:bfs",
    "family:bfs",
)

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("source", type=Path)
    p.add_argument("--out", type=Path, default=Path("compare-boxplot.png"))
    p.add_argument("--metric")
    p.add_argument("--include-warmup", action="store_true")
    p.add_argument("--sort", choices=("name", "median"), default="name")
    p.add_argument("--log", action="store_true")
    p.add_argument("--show-points", action="store_true")
    p.add_argument("--dpi", type=int, default=300)
    p.add_argument("--title", default="BFS execution time by cache configuration")
    return p.parse_args()

def available_metrics(samples):
    out = set()
    for s in samples:
        out.update(f"family:{k}" for k in s.families)
        out.update(f"event:{k}" for k in s.events)
    return out

def choose_metric(samples, requested):
    avail = available_metrics(samples)
    if requested:
        if requested not in avail:
            raise SystemExit(
                f"Metric {requested!r} not found.\nAvailable:\n  " +
                "\n  ".join(sorted(avail))
            )
        return requested
    for key in PREFERRED_METRICS:
        if key in avail:
            return key
    if not avail:
        raise SystemExit("No timing metrics found.")
    return sorted(avail)[0]

def metric_value(sample, metric):
    kind, name = metric.split(":", 1)
    mapping = sample.families if kind == "family" else sample.events
    value = mapping.get(name)
    if value is None:
        return None
    value = float(value)
    return value if math.isfinite(value) else None

def metric_label(metric):
    kind, name = metric.split(":", 1)
    return FAMILY_LABELS.get(name, name) if kind == "family" else name

def main():
    args = parse_args()
    samples = load_samples(args.source, include_server=False)
    if not samples:
        raise SystemExit(f"No comparable samples found under {args.source}")

    request_samples = [s for s in samples if s.source_kind == "request"]
    if request_samples:
        samples = request_samples

    if not args.include_warmup:
        samples = [s for s in samples if not s.label.lower().startswith("warmup-")]

    metric = choose_metric(samples, args.metric)

    grouped = defaultdict(list)
    for s in samples:
        value = metric_value(s, metric)
        if value is not None:
            grouped[s.run_type].append(value)

    grouped = {k: v for k, v in grouped.items() if v}
    if not grouped:
        raise SystemExit(f"No samples contain {metric}")

    if args.sort == "median":
        run_types = sorted(grouped, key=lambda x: float(np.median(grouped[x])))
    else:
        run_types = sorted(grouped)

    values = [grouped[r] for r in run_types]

    width = max(10.0, 1.25 * len(run_types) + 3.5)
    fig, ax = plt.subplots(figsize=(width, 6.5))

    ax.boxplot(values, tick_labels=run_types, showfliers=True, widths=0.55)

    if args.show_points:
        rng = np.random.default_rng(0)
        for x, ys in enumerate(values, start=1):
            jitter = rng.uniform(-0.10, 0.10, len(ys))
            ax.scatter(np.full(len(ys), x) + jitter, ys, s=14, alpha=0.45)

    if args.log:
        ax.set_yscale("log")

    ax.set_title(args.title)
    ax.set_ylabel(f"{metric_label(metric)} active wall-clock time [ms]")
    ax.set_xlabel("Cache configuration")
    ax.grid(axis="y", alpha=0.25)
    plt.setp(ax.get_xticklabels(), rotation=30, ha="right")

    fig.tight_layout()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=args.dpi, bbox_inches="tight")
    plt.close(fig)

    print(f"Metric: {metric}")
    print(f"Warmup included: {args.include_warmup}")
    for name in run_types:
        ys = grouped[name]
        print(
            f"{name}: n={len(ys)}, median={np.median(ys):.3f} ms, "
            f"min={min(ys):.3f} ms, max={max(ys):.3f} ms"
        )
    print(f"Saved: {args.out.resolve()}")

if __name__ == "__main__":
    main()
