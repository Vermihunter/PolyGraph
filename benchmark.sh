#!/usr/bin/env bash
set -Eeuo pipefail

# ============================================================
# PolyGraph SF1 benchmark bootstrap + run
#
# First recovery after native/full-replica experiments:
#   RELOAD_STORAGE=1 ./benchmark.sh
#
# Normal later runs (volumes already contain correct SF1 placement):
#   ./benchmark.sh
#
# Override repetitions/output/cache config as needed:
#   N=10 OUT=measurements/sf1-cache-N10 ./benchmark.sh
# ============================================================

SF_KEY="${SF_KEY:-sf1_0}"
DISTRIBUTION="${DISTRIBUTION:-schema-based}"
PROJECT="${PROJECT:-polygraph-sf1-0-schema-based}"

DOMAIN="${DOMAIN:-data/ldbc-snb/${SF_KEY}/${DISTRIBUTION}/domain.json}"
CFG="${CFG:-config/cache/benchmark/sf1/01-gpu-100.json}"
CACHE_ROOT="${CACHE_ROOT:-data/ldbc-snb/${SF_KEY}/${DISTRIBUTION}/caches}"
REQUEST_PLAN="${REQUEST_PLAN:-config/benchmarks/sf1-cache-http-requests.example.json}"

OUT="${OUT:-measurements/sf1-new-cache_dry-run}"
N="${N:-1}"
HTTP_PORT="${HTTP_PORT:-8080}"

# 1 only when you need to restore the canonical schema-based SF1 placement.
# IMPORTANT after the native full-replica Neo4j/Arango experiments.
RELOAD_STORAGE="${RELOAD_STORAGE:-0}"

# Recreate Arango each benchmark configuration so its process-level caches start
# from the same state, while preserving the persistent Docker volume.
RECREATE_ARANGO="${RECREATE_ARANGO:-1}"

# ------------------------------------------------------------
# Arango high-memory profile
# ------------------------------------------------------------

# Request scheduler (same as your previous setup)
export ARANGO_SERVER_MINIMAL_THREADS="${ARANGO_SERVER_MINIMAL_THREADS:-16}"
export ARANGO_SERVER_MAXIMAL_THREADS="${ARANGO_SERVER_MAXIMAL_THREADS:-64}"
export ARANGO_SERVER_IO_THREADS="${ARANGO_SERVER_IO_THREADS:-4}"
export ARANGO_SERVER_SCHEDULER_QUEUE_SIZE="${ARANGO_SERVER_SCHEDULER_QUEUE_SIZE:-4096}"
export ARANGO_SERVER_LOW_PRIORITY_MULTIPLIER="${ARANGO_SERVER_LOW_PRIORITY_MULTIPLIER:-2}"

# Storage/index caches
# 6 GiB RocksDB block cache
export ARANGO_ROCKSDB_BLOCK_CACHE_SIZE="${ARANGO_ROCKSDB_BLOCK_CACHE_SIZE:-6442450944}"
# 4 GiB Arango cache
export ARANGO_CACHE_SIZE="${ARANGO_CACHE_SIZE:-4294967296}"
# 1 GiB total RocksDB write buffers
export ARANGO_ROCKSDB_WRITE_BUFFER_SIZE="${ARANGO_ROCKSDB_WRITE_BUFFER_SIZE:-1073741824}"
export ARANGO_ROCKSDB_ENFORCE_CACHE_LIMIT="${ARANGO_ROCKSDB_ENFORCE_CACHE_LIMIT:-true}"

# AQL memory.
# These are separate from RocksDB/cache limits.
# 2 GiB per query
export ARANGO_QUERY_MEMORY_LIMIT="${ARANGO_QUERY_MEMORY_LIMIT:-2147483648}"
# 8 GiB total across simultaneously executing AQL queries
export ARANGO_QUERY_GLOBAL_MEMORY_LIMIT="${ARANGO_QUERY_GLOBAL_MEMORY_LIMIT:-8589934592}"
export ARANGO_QUERY_MEMORY_LIMIT_OVERRIDE="${ARANGO_QUERY_MEMORY_LIMIT_OVERRIDE:-true}"
export ARANGO_QUERY_LOG_MEMORY_THRESHOLD="${ARANGO_QUERY_LOG_MEMORY_THRESHOLD:-1073741824}"
export ARANGO_QUERY_MAX_RUNTIME="${ARANGO_QUERY_MAX_RUNTIME:-0}"
export ARANGO_QUERY_LOG_FAILED="${ARANGO_QUERY_LOG_FAILED:-true}"

# Keep global index-cache prefill disabled. Warm-up is performed by workload.
export ARANGO_ROCKSDB_AUTO_FILL_INDEX_CACHES_ON_STARTUP=false

COMPOSE=(
  docker compose
  --env-file storage_layer/.env
  -p "$PROJECT"
  -f storage_layer/docker-compose.yml
  -f storage_layer/docker-compose.arango-bfs-warm.yml
)

die() {
  echo "ERROR: $*" >&2
  exit 1
}

wait_service() {
  local service="$1"
  local timeout="${2:-180}"
  local start now cid state health

  cid="$("${COMPOSE[@]}" ps -q "$service")"
  [[ -n "$cid" ]] || die "Compose service '$service' has no container"

  start="$(date +%s)"
  while true; do
    state="$(docker inspect -f '{{.State.Status}}' "$cid" 2>/dev/null || true)"
    health="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "$cid" 2>/dev/null || true)"

    if [[ "$state" == "running" && ( "$health" == "healthy" || "$health" == "none" ) ]]; then
      echo "[storage] $service: state=$state health=$health"
      return 0
    fi

    if [[ "$state" == "exited" || "$state" == "dead" ]]; then
      echo "[storage] $service failed:" >&2
      docker logs --tail 100 "$cid" >&2 || true
      return 1
    fi

    now="$(date +%s)"
    if (( now - start >= timeout )); then
      echo "[storage] timeout waiting for $service (state=$state health=$health)" >&2
      docker logs --tail 100 "$cid" >&2 || true
      return 1
    fi

    sleep 1
  done
}

[[ -f "$DOMAIN" ]] || die "Missing $DOMAIN"
[[ -f "$CFG" ]] || die "Missing $CFG"
[[ -f "$REQUEST_PLAN" ]] || die "Missing $REQUEST_PLAN"

# Compose declares this network external.
docker network inspect polygraph-observability >/dev/null 2>&1 ||
  docker network create polygraph-observability >/dev/null

echo
echo "============================================================"
echo "POLYGRAPH SF1 STORAGE"
echo "============================================================"
echo "domain:              $DOMAIN"
echo "reload storage:      $RELOAD_STORAGE"
echo "Arango block cache:  $ARANGO_ROCKSDB_BLOCK_CACHE_SIZE"
echo "Arango cache:        $ARANGO_CACHE_SIZE"
echo "Arango write buffer: $ARANGO_ROCKSDB_WRITE_BUFFER_SIZE"
echo "AQL per-query:       $ARANGO_QUERY_MEMORY_LIMIT"
echo "AQL global:          $ARANGO_QUERY_GLOBAL_MEMORY_LIMIT"
echo

# Ensure the non-Arango persistent storage services are alive.
# Do NOT use `down -v`: these volumes contain the loaded graph.
"${COMPOSE[@]}" up -d neo4j postgresql

# Arango must be recreated for changed command-line memory settings to apply.
if [[ "$RECREATE_ARANGO" == "1" ]]; then
  "${COMPOSE[@]}" up -d --force-recreate --no-deps arangodb
else
  "${COMPOSE[@]}" up -d arangodb
fi

wait_service neo4j 240
wait_service postgresql 120
wait_service arangodb 240

ARANGO_CID="$("${COMPOSE[@]}" ps -q arangodb)"

echo
echo "=== Effective Arango memory/query options ==="
docker inspect "$ARANGO_CID" --format '{{json .Config.Cmd}}' |
  jq -r '.[]' |
  grep -E \
    'rocksdb.block-cache-size|cache.size|rocksdb.total-write-buffer-size|query.memory-limit|query.global-memory-limit|query.memory-limit-override|auto-fill-index-caches' ||
  true

echo
echo "=== Docker memory limit ==="
docker inspect "$ARANGO_CID" \
  --format 'Memory={{.HostConfig.Memory}} MemorySwap={{.HostConfig.MemorySwap}}'
echo "(Memory=0 means Docker is not imposing a hard RAM cap.)"

echo
echo "=== Host memory ==="
free -h || true

# The native full-replica experiments used the same database services.
# Restore the canonical SF1 schema-based physical placement when requested.
if [[ "$RELOAD_STORAGE" == "1" ]]; then
  echo
  echo "============================================================"
  echo "RELOADING CANONICAL SF1 SCHEMA-BASED STORAGE"
  echo "============================================================"

  python3 tools/polygraph.py instance "$SF_KEY" "$DISTRIBUTION" load

  # Loader may take a long time; confirm all storage processes survived.
  wait_service neo4j 240
  wait_service postgresql 120
  wait_service arangodb 240
fi

# The compact benchmark owns this HTTP port while it runs.
if command -v ss >/dev/null 2>&1 &&
   ss -H -ltn "sport = :$HTTP_PORT" 2>/dev/null | grep -q .; then
  echo >&2
  echo "Port $HTTP_PORT is already in use:" >&2
  ss -ltnp "sport = :$HTTP_PORT" >&2 || true
  die "Stop the stale PolyGraph HTTP server or set HTTP_PORT to another free port."
fi

NAME="$(basename "$CFG" .json)"
TMP="$OUT/.tmp-$NAME"

echo
echo "============================================================"
echo "POLYGRAPH BENCHMARK: $NAME"
echo "============================================================"

rm -rf "$TMP"

python3 tools/measurement/benchmark_compact_repeated.py \
  --domain "$DOMAIN" \
  --cache-configs "$CFG" \
  --cache-root "$CACHE_ROOT" \
  --request-plan "$REQUEST_PLAN" \
  --repetitions "$N" \
  --http-port "$HTTP_PORT" \
  --output-dir "$TMP" \
  --overwrite

# Only publish a completed benchmark directory.
[[ -d "$TMP/$NAME" ]] ||
  die "Benchmark returned successfully but expected output '$TMP/$NAME' is missing"

mkdir -p "$OUT"
rm -rf "$OUT/$NAME"
mv "$TMP/$NAME" "$OUT/$NAME"

if [[ ! -d "$OUT/_benchmark" && -d "$TMP/_benchmark" ]]; then
  mv "$TMP/_benchmark" "$OUT/_benchmark"
fi

rm -rf "$TMP"

echo
echo "============================================================"
echo "DONE"
echo "============================================================"
echo "Results: $OUT/$NAME"