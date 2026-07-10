#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.measurement.graph_bench.plotting import write_plots


def main() -> int:
    parser = argparse.ArgumentParser(description="Draw plots from tools/measurement/measure_bfs.py measurements.csv")
    parser.add_argument("--measurements-csv", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    outputs = write_plots(args.measurements_csv, args.output_dir)
    for path in outputs:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
