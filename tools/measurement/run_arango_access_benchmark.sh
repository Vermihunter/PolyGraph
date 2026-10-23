#!/usr/bin/env bash
set -euo pipefail

# Compare ArangoDB one-hop access primitives without changing the stored graph.
#
# A0 traversal is deliberately swept with small, bounded source batches. The
# normal SF1 request plan can use very large neighborBatchSize values and an
# automatic request strategy; feeding those settings directly to graph
# traversal can create one huge multi-source AQL traversal. A0 therefore has
# its own batch knob that does not alter Neo4j or A1 behavior.
#
# Default variants:
#   a0-traversal-8            graph traversal, 8 sources/query
#   a0-traversal-32           graph traversal, 32 sources/query
#   a0-traversal-64           graph traversal, 64 sources/query
#   a0-traversal-256          graph traversal, 256 sources/query
#   a0-traversal-1k           graph traversal, 1024 sources/query
#   a0-traversal-4k           graph traversal, 4096 sources/query
#   a1-edge-index             direct _from/_to edge-index lookups
#   a1-edge-index-plan-cache  A1 + AQL execution-plan cache
#
# Compatibility alias:
#   a0-traversal              same as a0-traversal-1k
#
# Usage:
#   tools/measurement/run_arango_access_benchmark.sh [repetitions]
#
# Optional:
#   ARANGO_ACCESS_BASE_CONFIG=config/benchmarks/sf1-mixed.json
#   ARANGO_ACCESS_OUT=measurements/sf1-arango-access
#   ARANGO_ACCESS_VARIANTS=a0-traversal-8,a0-traversal-32,a0-traversal-64,a0-traversal-256,a0-traversal-1k,a0-traversal-4k,a1-edge-index,a1-edge-index-plan-cache
#   ARANGO_ACCESS_REQUEST_TIMEOUT_SECONDS=600   # 0 disables timeout
#   ARANGO_ACCESS_PYTHON=python3
#   ARANGO_ACCESS_SKIP_STORAGE_ACTIVATE=1        # reuse already-running storage
#   ARANGO_ACCESS_COLD_START_EACH_CONFIG=1        # restart Arango before every cache config
#   ARANGO_ACCESS_COLD_PRIME_TIMEOUT_SECONDS=900  # unmeasured first BFS after restart; 0 disables
#   ARANGO_ACCESS_REVERSE_CACHE_ORDER=0            # 1 reverses the cache-config order

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

N="${1:-30}"
BASE_CONFIG="${ARANGO_ACCESS_BASE_CONFIG:-config/benchmarks/sf1-mixed.json}"
OUT_ROOT="${ARANGO_ACCESS_OUT:-measurements/sf1-arango-access}"
VARIANTS="${ARANGO_ACCESS_VARIANTS:-a0-traversal-256,a0-traversal-1k,a0-traversal-4k,a1-edge-index,a1-edge-index-plan-cache}"
REQUEST_TIMEOUT_SECONDS="${ARANGO_ACCESS_REQUEST_TIMEOUT_SECONDS:-600}"
PYTHON_BIN="${ARANGO_ACCESS_PYTHON:-python3}"
SKIP_STORAGE_ACTIVATE="${ARANGO_ACCESS_SKIP_STORAGE_ACTIVATE:-0}"
COLD_START_EACH_CONFIG="${ARANGO_ACCESS_COLD_START_EACH_CONFIG:-1}"
COLD_PRIME_TIMEOUT_SECONDS="${ARANGO_ACCESS_COLD_PRIME_TIMEOUT_SECONDS:-900}"
REVERSE_CACHE_ORDER="${ARANGO_ACCESS_REVERSE_CACHE_ORDER:-0}"

# Fail before a multi-hour benchmark if the Python environment cannot load Redis.
# Cache plans containing Redis are loaded by benchmark_cache_configs.py even when
# Redis itself is already running.
if ! "$PYTHON_BIN" -c "import redis" >/dev/null 2>&1; then
  cat >&2 <<EOF
[arango-access] Python Redis dependency is missing from: $PYTHON_BIN
Install the project tooling dependencies with:
  $PYTHON_BIN -m pip install -r tools/requirements.txt
Then rerun this command.
EOF
  exit 2
fi

mkdir -p "$OUT_ROOT/_generated"

# Storage is shared by every variant. Re-running `instance ... activate` before
# each variant is unnecessary and can race/stumble over published ports after a
# failed benchmark. Activate it once, then tell the inner matrix runner to reuse it.
if [[ "$SKIP_STORAGE_ACTIVATE" != "1" ]]; then
  read -r STORAGE_SF STORAGE_DISTRIBUTION < <("$PYTHON_BIN" - "$BASE_CONFIG" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1]).resolve()
cfg = json.loads(p.read_text())
print(cfg.get("scaleFactor", "1"), cfg.get("distribution", "schema-based"))
PY
  )
  echo "[arango-access] activating shared storage once: SF=$STORAGE_SF distribution=$STORAGE_DISTRIBUTION"
  if ! "$PYTHON_BIN" tools/polygraph.py instance "$STORAGE_SF" "$STORAGE_DISTRIBUTION" activate; then
    cat >&2 <<'EOF'
[arango-access] storage activation failed. If PostgreSQL port 5432 is occupied, inspect the owner with:
  docker ps --format "table {{.Names}}\t{{.Ports}}" | grep 5432 || true
  sudo ss -ltnp 'sport = :5432'
If the intended PolyGraph storage is already up and healthy, rerun with:
  ARANGO_ACCESS_SKIP_STORAGE_ACTIVATE=1 ...
EOF
    exit 2
  fi
fi

IFS=',' read -r -a requested <<< "$VARIANTS"
failed_variants=()

for variant in "${requested[@]}"; do
  traversal_batch_size="1024"
  case "$variant" in
    a0-traversal-8)
      mode="traversal"
      plan_cache="false"
      traversal_batch_size="8"
      ;;
    a0-traversal-32)
      mode="traversal"
      plan_cache="false"
      traversal_batch_size="32"
      ;;
    a0-traversal-64)
      mode="traversal"
      plan_cache="false"
      traversal_batch_size="64"
      ;;
    a0-traversal-256)
      mode="traversal"
      plan_cache="false"
      traversal_batch_size="256"
      ;;
    a0-traversal|a0-traversal-1k)
      mode="traversal"
      plan_cache="false"
      traversal_batch_size="1024"
      ;;
    a0-traversal-4k)
      mode="traversal"
      plan_cache="false"
      traversal_batch_size="4096"
      ;;
    a1-edge-index)
      mode="edge-index"
      plan_cache="false"
      ;;
    a1-edge-index-plan-cache)
      mode="edge-index"
      plan_cache="true"
      ;;
    *)
      echo "Unknown ARANGO_ACCESS_VARIANT: $variant" >&2
      exit 2
      ;;
  esac

  variant_out="$OUT_ROOT/$variant"
  generated_config="$OUT_ROOT/_generated/$variant.benchmark.json"
  generated_plan="$OUT_ROOT/_generated/$variant.requests.json"

  "$PYTHON_BIN" - "$BASE_CONFIG" "$generated_config" "$generated_plan" "$variant_out" "$mode" "$plan_cache" "$traversal_batch_size" "$REQUEST_TIMEOUT_SECONDS" "$COLD_START_EACH_CONFIG" "$COLD_PRIME_TIMEOUT_SECONDS" "$REVERSE_CACHE_ORDER" <<'PY'
import json
import pathlib
import sys

base_path = pathlib.Path(sys.argv[1]).resolve()
out_cfg = pathlib.Path(sys.argv[2]).resolve()
out_plan = pathlib.Path(sys.argv[3]).resolve()
out_dir = pathlib.Path(sys.argv[4]).resolve()
mode = sys.argv[5]
plan_cache = sys.argv[6].lower() == "true"
traversal_batch_size = int(sys.argv[7])
request_timeout_seconds = float(sys.argv[8])
cold_start_each_config = sys.argv[9].strip().lower() not in {'0', 'false', 'no', 'off'}
cold_prime_timeout_seconds = float(sys.argv[10])
reverse_cache_order = sys.argv[11].strip().lower() in {'1', 'true', 'yes', 'on'}
if request_timeout_seconds < 0:
    raise ValueError("ARANGO_ACCESS_REQUEST_TIMEOUT_SECONDS must be >= 0")
if cold_prime_timeout_seconds < 0:
    raise ValueError("ARANGO_ACCESS_COLD_PRIME_TIMEOUT_SECONDS must be >= 0")

root = pathlib.Path.cwd().resolve()
config = json.loads(base_path.read_text())
request_path = pathlib.Path(config["requestPlan"])
if not request_path.is_absolute():
    request_path = root / request_path
plan = json.loads(request_path.read_text())

payload = plan.setdefault("defaults", {}).setdefault("payload", {})
payload["arangoExpandMode"] = mode
payload["arangoUsePlanCache"] = plan_cache
payload["arangoTraversalBatchSize"] = traversal_batch_size

# A0 must stay bounded even when the base benchmark uses Auto for A1. This is
# intentionally Arango-specific: neighborBatchSize remains untouched, so Neo4j
# and every other backend see exactly the same request settings in all variants.
if mode == "traversal":
    payload["arangoRequestStrategy"] = "chunked"

out_plan.parent.mkdir(parents=True, exist_ok=True)
out_plan.write_text(json.dumps(plan, indent=2) + "\n")
config["requestPlan"] = str(out_plan)
config["outputDir"] = str(out_dir)
config["httpRequestTimeoutSeconds"] = request_timeout_seconds
config["coldRestartArangoBeforeEachCache"] = cold_start_each_config
config["arangoColdPrimeTimeoutSeconds"] = cold_prime_timeout_seconds if cold_start_each_config else 0.0
config["reverseCacheOrder"] = reverse_cache_order
out_cfg.write_text(json.dumps(config, indent=2) + "\n")
PY

  echo
  echo "============================================================"
  echo "ARANGO ACCESS BENCHMARK: $variant"
  echo "============================================================"
  echo "mode:                 $mode"
  echo "plan cache:           $plan_cache"
  echo "A0 traversal batch:   $traversal_batch_size"
  echo "HTTP request timeout: ${REQUEST_TIMEOUT_SECONDS}s (0 = disabled)"
  echo "Arango cold reset:      $COLD_START_EACH_CONFIG (before every cache config)"
  echo "Arango cold prime:      ${COLD_PRIME_TIMEOUT_SECONDS}s timeout (unmeasured; 0 = disabled)"
  echo "cache order:            $([[ "$REVERSE_CACHE_ORDER" == "1" ]] && echo reverse || echo forward)"
  if [[ "$mode" == "traversal" ]]; then
    echo "request strategy:     chunked (forced for bounded A0)"
  else
    echo "request strategy:     inherited from base request plan"
  fi
  echo "output:               $variant_out"

  status="success"
  if ! PYTHON_BIN="$PYTHON_BIN" SKIP_ACTIVATE=1 CFG="$generated_config" OUT="$variant_out" tools/measurement/benchmark.sh "$N"; then
    status="failed"
    failed_variants+=("$variant")
    echo "[arango-access] $variant failed or timed out; continuing with the remaining variants." >&2
  fi

  mkdir -p "$variant_out"
  cat > "$variant_out/arango-access.env" <<META
variant=$variant
status=$status
arango_expand_mode=$mode
arango_use_plan_cache=$plan_cache
arango_traversal_batch_size=$traversal_batch_size
http_request_timeout_seconds=$REQUEST_TIMEOUT_SECONDS
cold_restart_arango_before_each_cache=$COLD_START_EACH_CONFIG
cold_prime_timeout_seconds=$COLD_PRIME_TIMEOUT_SECONDS
reverse_cache_order=$REVERSE_CACHE_ORDER
repetitions=$N
base_config=$BASE_CONFIG
META
done

printf '\nBenchmark variants finished. Results are under %s\n' "$OUT_ROOT"
printf 'Generated request plans are under %s/_generated\n' "$OUT_ROOT"

if "$PYTHON_BIN" tools/measurement/analyze_arango_access_benchmark.py --root "$OUT_ROOT"; then
  printf 'Summary: %s/_analysis/summary.md\n' "$OUT_ROOT"
else
  echo "[arango-access] analysis could not be produced; raw variant outputs are preserved." >&2
fi

if (( ${#failed_variants[@]} > 0 )); then
  printf '\nThe following variants failed/timed out, but all requested variants were attempted:\n' >&2
  printf '  - %s\n' "${failed_variants[@]}" >&2
  exit 1
fi
