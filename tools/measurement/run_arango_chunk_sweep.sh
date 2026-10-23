#!/usr/bin/env bash
set -euo pipefail

# Sweep ArangoDB A1 edge-index chunk sizes while pinning the Arango request
# scheduler to a fixed number of threads. By default PolyGraph's per-query
# Arango concurrency is matched to the same value.
#
# Usage:
#   tools/measurement/run_arango_chunk_sweep.sh [repetitions]
#
# Useful overrides:
#   ARANGO_SWEEP_THREADS=16
#   ARANGO_SWEEP_CONCURRENCY=16
#   ARANGO_SWEEP_CHUNK_SIZES=1024,4096,16384,65536,250000
#   ARANGO_SWEEP_CACHE_NAMES=04-gpu30-redis20-mmap20,05-gpu20-redis15-mmap15,08-no-cache
#   ARANGO_SWEEP_PASSES=2
#   ARANGO_SWEEP_OUT=measurements/my-arango-sweep
#   ARANGO_SWEEP_REQUEST_TIMEOUT_SECONDS=900
#   ARANGO_SWEEP_SKIP_STORAGE_ACTIVATE=1
#   ARANGO_SWEEP_PYTHON=/home/akos/miniconda3/bin/python3
#
# A1 edge-index + plan cache is forced, and arangoRequestStrategy=chunked is
# forced so neighborBatchSize really is the Arango chunk size for every level.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

REPETITIONS="${1:-5}"
THREADS="${ARANGO_SWEEP_THREADS:-16}"
CONCURRENCY="${ARANGO_SWEEP_CONCURRENCY:-$THREADS}"
CHUNK_SIZES_CSV="${ARANGO_SWEEP_CHUNK_SIZES:-1024,4096,16384,65536,250000}"
CACHE_NAMES_CSV="${ARANGO_SWEEP_CACHE_NAMES:-04-gpu30-redis20-mmap20,05-gpu20-redis15-mmap15,08-no-cache}"
PASSES="${ARANGO_SWEEP_PASSES:-1}"
BASE_PLAN="${ARANGO_SWEEP_BASE_PLAN:-config/benchmarks/sf1-bfs-classic-http-requests.json}"
OUT_ROOT="${ARANGO_SWEEP_OUT:-measurements/sf1-arango-chunk-sweep-$(date +%Y%m%d-%H%M%S)}"
REQUEST_TIMEOUT_SECONDS="${ARANGO_SWEEP_REQUEST_TIMEOUT_SECONDS:-900}"
SKIP_STORAGE_ACTIVATE="${ARANGO_SWEEP_SKIP_STORAGE_ACTIVATE:-0}"
PYTHON_BIN="${ARANGO_SWEEP_PYTHON:-${PYTHON_BIN:-python3}}"
DOMAIN="${ARANGO_SWEEP_DOMAIN:-data/ldbc-snb/sf1_0/schema-based/domain.json}"
CACHE_ROOT="${ARANGO_SWEEP_CACHE_ROOT:-data/ldbc-snb/sf1_0/schema-based/caches}"
HTTP_PORT="${ARANGO_SWEEP_HTTP_PORT:-8080}"
PROJECT_NAME="${ARANGO_SWEEP_PROJECT_NAME:-polygraph-sf1-0-schema-based}"
REDIS_ENV="${ARANGO_SWEEP_REDIS_ENV:-storage_layer/redis/.env.sf1-benchmark}"
OVERWRITE="${ARANGO_SWEEP_OVERWRITE:-0}"

fail() {
  echo "[arango-chunk-sweep] ERROR: $*" >&2
  exit 2
}

[[ "$REPETITIONS" =~ ^[1-9][0-9]*$ ]] || fail "repetitions must be a positive integer"
[[ "$THREADS" =~ ^[1-9][0-9]*$ ]] || fail "ARANGO_SWEEP_THREADS must be a positive integer"
[[ "$CONCURRENCY" =~ ^[1-9][0-9]*$ ]] || fail "ARANGO_SWEEP_CONCURRENCY must be a positive integer"
[[ "$PASSES" =~ ^[1-9][0-9]*$ ]] || fail "ARANGO_SWEEP_PASSES must be a positive integer"
[[ -f "$BASE_PLAN" ]] || fail "missing base request plan: $BASE_PLAN"
[[ -f "$DOMAIN" ]] || fail "missing domain: $DOMAIN"

IFS=',' read -r -a CHUNK_SIZES <<< "$CHUNK_SIZES_CSV"
IFS=',' read -r -a CACHE_NAMES <<< "$CACHE_NAMES_CSV"
((${#CHUNK_SIZES[@]} > 0)) || fail "no chunk sizes selected"
((${#CACHE_NAMES[@]} > 0)) || fail "no cache configs selected"

for chunk in "${CHUNK_SIZES[@]}"; do
  [[ "$chunk" =~ ^[1-9][0-9]*$ ]] || fail "invalid chunk size: $chunk"
done
for cache in "${CACHE_NAMES[@]}"; do
  [[ -f "$CACHE_ROOT/$cache/cache-plan.json" ]] || fail "missing cache plan: $CACHE_ROOT/$cache/cache-plan.json"
done

if [[ -e "$OUT_ROOT" && "$OVERWRITE" != "1" ]]; then
  fail "output already exists: $OUT_ROOT (set ARANGO_SWEEP_OVERWRITE=1 or choose ARANGO_SWEEP_OUT)"
fi
if [[ -e "$OUT_ROOT" && "$OVERWRITE" == "1" ]]; then
  rm -rf "$OUT_ROOT"
fi
mkdir -p "$OUT_ROOT/_generated" "$OUT_ROOT/logs"

# storage_layer/docker-compose.yml already wires these environment variables to
# --server.minimal-threads and --server.maximal-threads. Setting both to the
# same value pins the Arango scheduler pool to exactly THREADS threads.
export ARANGO_SERVER_MINIMAL_THREADS="$THREADS"
export ARANGO_SERVER_MAXIMAL_THREADS="$THREADS"

if [[ "$SKIP_STORAGE_ACTIVATE" != "1" ]]; then
  echo "[arango-chunk-sweep] activating SF1 schema-based storage"
  "$PYTHON_BIN" tools/polygraph.py instance 1 schema-based activate
fi

COMPOSE=(
  docker compose
  --env-file storage_layer/.env
  -f storage_layer/docker-compose.yml
  --project-name "$PROJECT_NAME"
)

# Recreate only ArangoDB so the requested scheduler values are guaranteed to be
# present in the running process. Named data volumes are preserved.
echo "[arango-chunk-sweep] recreating ArangoDB with scheduler threads=$THREADS"
"${COMPOSE[@]}" up -d --force-recreate --wait arangodb

docker compose \
  --env-file "$REDIS_ENV" \
  -f storage_layer/redis/docker-compose.yml \
  up -d --wait

ARANGO_CID="$("${COMPOSE[@]}" ps -q arangodb)"
[[ -n "$ARANGO_CID" ]] || fail "could not resolve running ArangoDB container"
ARANGO_CMD="$(docker inspect --format '{{json .Config.Cmd}}' "$ARANGO_CID")"
echo "[arango-chunk-sweep] Arango container: $ARANGO_CID"
echo "[arango-chunk-sweep] command: $ARANGO_CMD"
if [[ "$ARANGO_CMD" != *"--server.minimal-threads=$THREADS"* || "$ARANGO_CMD" != *"--server.maximal-threads=$THREADS"* ]]; then
  fail "running ArangoDB command does not contain the requested fixed thread count ($THREADS)"
fi

cat > "$OUT_ROOT/sweep.env" <<META
format=polygraph.arango-chunk-sweep.v1
threads=$THREADS
arango_max_concurrent_requests=$CONCURRENCY
chunk_sizes=$CHUNK_SIZES_CSV
cache_names=$CACHE_NAMES_CSV
passes=$PASSES
repetitions=$REPETITIONS
base_plan=$BASE_PLAN
domain=$DOMAIN
request_timeout_seconds=$REQUEST_TIMEOUT_SECONDS
arango_expand_mode=edge-index
arango_use_plan_cache=true
arango_request_strategy=chunked
META

# Generate one HTTP request plan per chunk size by overriding only the Arango
# experiment knobs in the existing classic BFS request plan.
for chunk in "${CHUNK_SIZES[@]}"; do
  "$PYTHON_BIN" - "$BASE_PLAN" "$OUT_ROOT/_generated/chunk-$chunk.requests.json" "$chunk" "$CONCURRENCY" <<'PY'
import json
import pathlib
import sys

src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
chunk = int(sys.argv[3])
concurrency = int(sys.argv[4])

data = json.loads(src.read_text(encoding="utf-8"))
payload = data.setdefault("defaults", {}).setdefault("payload", {})
payload["allowNativePushdown"] = False
payload["executionPolicy"] = "classic"
payload["arangoExpandMode"] = "edge-index"
payload["arangoUsePlanCache"] = True
payload["arangoRequestStrategy"] = "chunked"
payload["arangoMaxConcurrentRequests"] = concurrency
payload["neighborBatchSize"] = chunk

dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
PY
done

printf 'pass\tchunk_size\tcache_name\tstatus\texit_code\toutput_dir\n' > "$OUT_ROOT/runs.tsv"
failed=0

run_one() {
  local pass_no="$1"
  local chunk="$2"
  local cache="$3"
  local pass_name
  pass_name="$(printf 'pass-%02d' "$pass_no")"
  local out="$OUT_ROOT/$pass_name/chunk-$chunk/$cache"
  local log="$OUT_ROOT/logs/${pass_name}-chunk-${chunk}-${cache}.log"

  mkdir -p "$(dirname "$out")"
  echo
  echo "============================================================"
  echo "ARANGO CHUNK SWEEP: $pass_name / chunk=$chunk / $cache"
  echo "============================================================"

  set +e
  "$PYTHON_BIN" tools/measurement/benchmark_compact_repeated.py \
    --domain "$DOMAIN" \
    --cache-plans "$CACHE_ROOT/$cache/cache-plan.json" \
    --request-plan "$OUT_ROOT/_generated/chunk-$chunk.requests.json" \
    --repetitions "$REPETITIONS" \
    --output-dir "$out" \
    --http-port "$HTTP_PORT" \
    --http-startup-timeout 0.0 \
    --http-request-timeout "$REQUEST_TIMEOUT_SECONDS" \
    --cold-prime-request-timeout "$REQUEST_TIMEOUT_SECONDS" \
    --overwrite 2>&1 | tee "$log"
  local rc=${PIPESTATUS[0]}
  set -e

  local status="success"
  if ((rc != 0)); then
    status="failed"
    failed=1
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$pass_name" "$chunk" "$cache" "$status" "$rc" "$out" >> "$OUT_ROOT/runs.tsv"
}

for ((pass_no=1; pass_no<=PASSES; ++pass_no)); do
  if ((pass_no % 2 == 1)); then
    for chunk in "${CHUNK_SIZES[@]}"; do
      for cache in "${CACHE_NAMES[@]}"; do
        run_one "$pass_no" "$chunk" "$cache"
      done
    done
  else
    # Reverse both axes on even passes to reduce simple temporal/order bias.
    for ((ci=${#CHUNK_SIZES[@]}-1; ci>=0; --ci)); do
      for ((ki=${#CACHE_NAMES[@]}-1; ki>=0; --ki)); do
        run_one "$pass_no" "${CHUNK_SIZES[$ci]}" "${CACHE_NAMES[$ki]}"
      done
    done
  fi
done

echo
echo "[arango-chunk-sweep] analyzing results"
"$PYTHON_BIN" tools/measurement/analyze_arango_chunk_sweep.py --root "$OUT_ROOT"

echo
echo "============================================================"
echo "ARANGO CHUNK SWEEP FINISHED"
echo "============================================================"
echo "threads:       $THREADS"
echo "concurrency:   $CONCURRENCY"
echo "chunk sizes:   $CHUNK_SIZES_CSV"
echo "output:        $OUT_ROOT"
echo "summary:       $OUT_ROOT/_analysis/summary.md"
echo "aggregate CSV: $OUT_ROOT/_analysis/aggregate.csv"

if ((failed != 0)); then
  echo "[arango-chunk-sweep] one or more runs failed; successful runs were preserved and analyzed" >&2
  exit 1
fi
