#!/usr/bin/env bash
set -uo pipefail

# Run the full SF1 cache matrix with both BFS execution policies:
#   1) classic
#   2) direction-optimized
#
# Usage:
#   tools/measurement/run_bfs_policy_matrix.sh [repetitions]
#
# Useful overrides:
#   PYTHON_BIN=/home/akos/miniconda3/bin/python3
#   SKIP_ACTIVATE=1                  # storage is already running
#   SKIP_REDIS_START=1               # Redis is already running
#   OUT_ROOT=measurements/my-run
#   BASE_CFG=config/benchmarks/sf1-cache-matrix.json

N="${1:-10}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
BASE_CFG="${BASE_CFG:-config/benchmarks/sf1-cache-matrix.json}"
CLASSIC_REQUESTS="${CLASSIC_REQUESTS:-config/benchmarks/sf1-bfs-classic-http-requests.json}"
OPT_REQUESTS="${OPT_REQUESTS:-config/benchmarks/sf1-bfs-direction-optimized-http-requests.json}"
OUT_ROOT="${OUT_ROOT:-measurements/sf1-bfs-policy-all-$(date +%Y%m%d-%H%M%S)}"
SKIP_ACTIVATE="${SKIP_ACTIVATE:-0}"
SKIP_REDIS_START="${SKIP_REDIS_START:-0}"

for f in "$BASE_CFG" "$CLASSIC_REQUESTS" "$OPT_REQUESTS"; do
  if [[ ! -f "$f" ]]; then
    echo "error: missing $f" >&2
    exit 2
  fi
done

mkdir -p "$OUT_ROOT/_generated"
CLASSIC_CFG="$OUT_ROOT/_generated/classic.matrix.json"
OPT_CFG="$OUT_ROOT/_generated/direction-optimized.matrix.json"

"$PYTHON_BIN" - "$BASE_CFG" "$CLASSIC_REQUESTS" "$OPT_REQUESTS" \
  "$CLASSIC_CFG" "$OPT_CFG" "$OUT_ROOT" <<'PY'
import glob
import json
import sys
from pathlib import Path

base_path, classic_req, opt_req, classic_out, opt_out, out_root = sys.argv[1:]
with open(base_path, "r", encoding="utf-8") as f:
    base = json.load(f)

variants = []
for policy, request_plan, destination, subdir in (
    ("classic", classic_req, classic_out, "classic"),
    ("direction-optimized", opt_req, opt_out, "direction-optimized"),
):
    cfg = dict(base)
    cfg["requestPlan"] = request_plan
    cfg["outputDir"] = str(Path(out_root) / subdir)
    with open(destination, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")

cache_glob = base.get("cacheConfigGlob")
if cache_glob:
    matches = sorted(glob.glob(cache_glob))
    print(f"[bfs-policy] cache configs ({len(matches)}):")
    for p in matches:
        print(f"  - {Path(p).stem}")
    if not matches:
        raise SystemExit(f"No cache configs matched: {cache_glob}")
PY

echo
echo "============================================================"
echo "BFS POLICY MATRIX"
echo "============================================================"
echo "repetitions:    $N"
echo "output root:    $OUT_ROOT"
echo "classic plan:   $CLASSIC_REQUESTS"
echo "optimized plan: $OPT_REQUESTS"
echo

classic_status=0
optimized_status=0

# If storage is not already running, let the first matrix activate it once.
# The second matrix reuses that storage state instead of recreating it.
classic_skip_activate="$SKIP_ACTIVATE"
optimized_skip_activate=1

run_matrix() {
  local name="$1"
  local cfg="$2"
  local out="$3"
  local skip_activate="$4"

  echo
  echo "============================================================"
  echo "RUNNING: $name"
  echo "============================================================"

  SKIP_ACTIVATE="$skip_activate" \
  SKIP_REDIS_START="$SKIP_REDIS_START" \
  PYTHON_BIN="$PYTHON_BIN" \
  CFG="$cfg" \
  OUT="$out" \
    tools/measurement/benchmark.sh "$N"
}

if ! run_matrix \
    "classic" \
    "$CLASSIC_CFG" \
    "$OUT_ROOT/classic" \
    "$classic_skip_activate"; then
  classic_status=$?
  # `! command` normalizes $? in bash, so retain a non-zero marker explicitly.
  classic_status=1
  echo "[bfs-policy] classic matrix FAILED; continuing with direction-optimized." >&2
fi

if ! run_matrix \
    "direction-optimized" \
    "$OPT_CFG" \
    "$OUT_ROOT/direction-optimized" \
    "$optimized_skip_activate"; then
  optimized_status=1
  echo "[bfs-policy] direction-optimized matrix FAILED." >&2
fi

echo
echo "============================================================"
echo "FINISHED"
echo "============================================================"
echo "classic:            $OUT_ROOT/classic"
echo "direction-optimized: $OUT_ROOT/direction-optimized"
echo "generated configs:  $OUT_ROOT/_generated"

if (( classic_status != 0 || optimized_status != 0 )); then
  exit 1
fi
