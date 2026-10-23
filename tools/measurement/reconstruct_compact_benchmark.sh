#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "usage: $0 BENCHMARK_DIR [METRICS_CONFIG] [OUTPUT_DIR]" >&2
  exit 2
fi

BENCHMARK_DIR="$(realpath "$1")"
METRICS_CONFIG="${2:-config/measurement/prometheus-paper.example.json}"
OUTPUT_DIR="${3:-$BENCHMARK_DIR/reconstruction}"

ARGS=(
  "$BENCHMARK_DIR"
  --out-dir "$OUTPUT_DIR"
  --split-by-run-type
  --with-compare
  --inline-plotly
)

if [[ -n "$METRICS_CONFIG" ]]; then
  ARGS+=(--metrics-config "$METRICS_CONFIG" --refresh-metrics)
fi

python3 tools/measurement/log_reconstruction.py "${ARGS[@]}"

echo
echo "Hub:        $OUTPUT_DIR/index.html"
echo "Comparison: $OUTPUT_DIR/compare.html"
echo "Per-cache dashboards:"
find "$OUTPUT_DIR/runs" -name dashboard.html -print 2>/dev/null | sort || true
echo "Metrics pages:"
find "$OUTPUT_DIR/runs" -path '*/metrics/*' -name '*.html' -print 2>/dev/null | sort || true
