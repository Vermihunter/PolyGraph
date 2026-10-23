#!/usr/bin/env bash
set -euo pipefail

# Long, order-balanced ArangoDB experiment for the practical PolyGraph A1 path.
#
# The suite deliberately does not spend the night on full-BFS A0 traversal: the
# tested A0 batch sizes (8..4096) already fail to finish the cold first BFS within
# the configured deadlines. Instead it collects enough A1/A1-plan-cache samples
# for per-root distributions, plan-cache effects, cache-placement effects, and
# cold-prime costs.
#
# Default design (15 repetitions/pass):
#   pass 1: A1              cache order forward
#   pass 2: A1 + plan cache cache order reverse
#   pass 3: A1 + plan cache cache order forward
#   pass 4: A1              cache order reverse
#
# Every pass restarts Arango before every cache configuration and runs one
# unmeasured cold-prime BFS. The two passes per variant yield 30 measured rounds
# per cache/variant (90 measured BFS requests with the default 3-root plan).
# Reversing cache order and variant order reduces systematic first/last-run bias.
#
# Usage:
#   tools/measurement/run_arango_overnight_benchmark.sh [repetitions_per_pass]
#
# Useful overrides:
#   ARANGO_OVERNIGHT_BASE_CONFIG=config/benchmarks/sf1-arango-overnight.json
#   ARANGO_OVERNIGHT_OUT=measurements/sf1-arango-overnight
#   ARANGO_OVERNIGHT_REQUEST_TIMEOUT_SECONDS=300
#   ARANGO_OVERNIGHT_COLD_PRIME_TIMEOUT_SECONDS=900
#   ARANGO_OVERNIGHT_SKIP_STORAGE_ACTIVATE=1
#   ARANGO_OVERNIGHT_PYTHON=/home/akos/miniconda3/bin/python3
#   ARANGO_OVERNIGHT_OVERWRITE=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

REPS_PER_PASS="${1:-${ARANGO_OVERNIGHT_REPS_PER_PASS:-15}}"
BASE_CONFIG="${ARANGO_OVERNIGHT_BASE_CONFIG:-config/benchmarks/sf1-arango-overnight.json}"
PYTHON_BIN="${ARANGO_OVERNIGHT_PYTHON:-python3}"
REQUEST_TIMEOUT_SECONDS="${ARANGO_OVERNIGHT_REQUEST_TIMEOUT_SECONDS:-300}"
COLD_PRIME_TIMEOUT_SECONDS="${ARANGO_OVERNIGHT_COLD_PRIME_TIMEOUT_SECONDS:-900}"
SKIP_STORAGE_ACTIVATE="${ARANGO_OVERNIGHT_SKIP_STORAGE_ACTIVATE:-1}"
OVERWRITE="${ARANGO_OVERNIGHT_OVERWRITE:-0}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
OUT_ROOT="${ARANGO_OVERNIGHT_OUT:-measurements/sf1-arango-overnight-${TIMESTAMP}}"

if ! [[ "$REPS_PER_PASS" =~ ^[1-9][0-9]*$ ]]; then
  echo "[arango-overnight] repetitions_per_pass must be a positive integer" >&2
  exit 2
fi
if [[ ! -f "$BASE_CONFIG" ]]; then
  echo "[arango-overnight] base config not found: $BASE_CONFIG" >&2
  exit 2
fi
if ! "$PYTHON_BIN" -c 'import redis, matplotlib' >/dev/null 2>&1; then
  cat >&2 <<EOF2
[arango-overnight] Python tooling dependencies are missing from: $PYTHON_BIN
Install them with:
  $PYTHON_BIN -m pip install -r tools/requirements.txt
EOF2
  exit 2
fi

if [[ -e "$OUT_ROOT" && "$OVERWRITE" != "1" ]]; then
  echo "[arango-overnight] output already exists: $OUT_ROOT" >&2
  echo "Set ARANGO_OVERNIGHT_OVERWRITE=1 or choose ARANGO_OVERNIGHT_OUT." >&2
  exit 2
fi
if [[ -e "$OUT_ROOT" && "$OVERWRITE" == "1" ]]; then
  rm -rf "$OUT_ROOT"
fi
mkdir -p "$OUT_ROOT/logs"

# Activate the shared persistent storage at most once. Child passes always reuse
# it so Compose activation cannot perturb the experiment between passes.
if [[ "$SKIP_STORAGE_ACTIVATE" != "1" ]]; then
  read -r STORAGE_SF STORAGE_DISTRIBUTION < <("$PYTHON_BIN" - "$BASE_CONFIG" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1]).resolve()
cfg = json.loads(p.read_text())
print(cfg.get("scaleFactor", "1"), cfg.get("distribution", "schema-based"))
PY
  )
  echo "[arango-overnight] activating shared storage once: SF=$STORAGE_SF distribution=$STORAGE_DISTRIBUTION"
  "$PYTHON_BIN" tools/polygraph.py instance "$STORAGE_SF" "$STORAGE_DISTRIBUTION" activate
fi

TOTAL_ROUNDS_PER_VARIANT=$((2 * REPS_PER_PASS))
cat > "$OUT_ROOT/overnight.env" <<META
format=polygraph.arango-overnight
repetitions_per_pass=$REPS_PER_PASS
measured_rounds_per_variant_cache=$TOTAL_ROUNDS_PER_VARIANT
base_config=$BASE_CONFIG
request_timeout_seconds=$REQUEST_TIMEOUT_SECONDS
cold_prime_timeout_seconds=$COLD_PRIME_TIMEOUT_SECONDS
cold_restart_arango_before_each_cache=1
cache_order_balanced=1
variant_order_balanced=1
host_page_cache_dropped=0
META

cat <<EOF2
============================================================
POLYGRAPH ARANGO OVERNIGHT BENCHMARK
============================================================
repetitions/pass:      $REPS_PER_PASS
rounds/variant/cache:  $TOTAL_ROUNDS_PER_VARIANT
normal timeout:        ${REQUEST_TIMEOUT_SECONDS}s
cold-prime timeout:    ${COLD_PRIME_TIMEOUT_SECONDS}s
cold Arango restart:   before every cache configuration
host page cache:       preserved
output:                $OUT_ROOT

Pass schedule:
  1. A1              forward cache order
  2. A1 + plan cache reverse cache order
  3. A1 + plan cache forward cache order
  4. A1              reverse cache order
============================================================
EOF2

failed_passes=()

run_pass() {
  local pass_name="$1"
  local variant="$2"
  local reverse_order="$3"
  local pass_out="$OUT_ROOT/$pass_name"
  local log="$OUT_ROOT/logs/$pass_name.log"

  echo
  echo "############################################################"
  echo "# $pass_name"
  echo "# variant=$variant reverse_cache_order=$reverse_order"
  echo "############################################################"

  set +e
  ARANGO_ACCESS_BASE_CONFIG="$BASE_CONFIG" \
  ARANGO_ACCESS_OUT="$pass_out" \
  ARANGO_ACCESS_VARIANTS="$variant" \
  ARANGO_ACCESS_REQUEST_TIMEOUT_SECONDS="$REQUEST_TIMEOUT_SECONDS" \
  ARANGO_ACCESS_COLD_START_EACH_CONFIG=1 \
  ARANGO_ACCESS_COLD_PRIME_TIMEOUT_SECONDS="$COLD_PRIME_TIMEOUT_SECONDS" \
  ARANGO_ACCESS_REVERSE_CACHE_ORDER="$reverse_order" \
  ARANGO_ACCESS_SKIP_STORAGE_ACTIVATE=1 \
  ARANGO_ACCESS_PYTHON="$PYTHON_BIN" \
    tools/measurement/run_arango_access_benchmark.sh "$REPS_PER_PASS" 2>&1 | tee "$log"
  local rc=${PIPESTATUS[0]}
  set -e

  if (( rc != 0 )); then
    failed_passes+=("$pass_name")
    echo "[arango-overnight] $pass_name failed (rc=$rc); continuing so later passes still run." >&2
  fi
}

# ABBA-like ordering: both execution variants and cache order appear once in
# each temporal half of the experiment.
run_pass "pass-01-a1-forward"  "a1-edge-index"            0
run_pass "pass-02-a1pc-reverse" "a1-edge-index-plan-cache" 1
run_pass "pass-03-a1pc-forward" "a1-edge-index-plan-cache" 0
run_pass "pass-04-a1-reverse"  "a1-edge-index"            1

echo
echo "[arango-overnight] building merged statistics and plots..."
"$PYTHON_BIN" tools/measurement/analyze_arango_overnight_benchmark.py --root "$OUT_ROOT"

echo
echo "============================================================"
echo "OVERNIGHT BENCHMARK FINISHED"
echo "============================================================"
echo "Summary:     $OUT_ROOT/_analysis/summary.md"
echo "Statistics:  $OUT_ROOT/_analysis/statistics.csv"
echo "Per root:    $OUT_ROOT/_analysis/per_root.csv"
echo "Cold prime:  $OUT_ROOT/_analysis/cold_prime.csv"
echo "Pairwise:    $OUT_ROOT/_analysis/pairwise.csv"
echo "Plots:       $OUT_ROOT/_analysis/*.png"

if (( ${#failed_passes[@]} > 0 )); then
  printf '[arango-overnight] incomplete passes:\n' >&2
  printf '  - %s\n' "${failed_passes[@]}" >&2
  exit 1
fi
