#!/usr/bin/env bash
set -euo pipefail

# Convenience wrapper for the cache benchmark runner.
#
# Usage:
#   tools/measurement/benchmark.sh [repetitions]
#
# Optional environment overrides:
#   CFG=config/benchmarks/sf1-gpu-transport.json
#   OUT=measurements/sf1-gpu-transport/v0-baseline
#
# These overrides are used by run_gpu_transport_matrix.sh so every GPU
# implementation is benchmarked against exactly the cache plan selected by CFG.

N="${1:-10}"
CFG="${CFG:-config/benchmarks/sf1-cache-matrix.json}"
OUT="${OUT:-}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
SKIP_ACTIVATE="${SKIP_ACTIVATE:-0}"
SKIP_REDIS_START="${SKIP_REDIS_START:-0}"

args=(
  "$PYTHON_BIN" tools/measurement/run_cache_matrix.py
  --config "$CFG"
  --repetitions "$N"
  --overwrite
)

if [[ -n "$OUT" ]]; then
  args+=(--output-dir "$OUT")
fi

if [[ "$SKIP_ACTIVATE" == "1" ]]; then
  args+=(--skip-activate)
fi

if [[ "$SKIP_REDIS_START" == "1" ]]; then
  args+=(--skip-redis-start)
fi

exec "${args[@]}"
