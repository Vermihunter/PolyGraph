#!/usr/bin/env bash
set -euo pipefail

PROJECT="${1:-polygraph-sf1-0-schema-based}"
SNAPSHOT="${2:-}"

# Defaults intentionally represent the steady-state BFS profile. All can be
# overridden from the calling shell for memory-sensitivity experiments.
export ARANGO_CACHE_SIZE="${ARANGO_CACHE_SIZE:-6442450944}"                       # 6 GiB
export ARANGO_CACHE_HIGH_WATER_MULTIPLIER="${ARANGO_CACHE_HIGH_WATER_MULTIPLIER:-0.90}"
export ARANGO_CACHE_MAX_VALUE_SIZE="${ARANGO_CACHE_MAX_VALUE_SIZE:-67108864}"     # 64 MiB
export ARANGO_CACHE_MIN_VALUE_SIZE_FOR_EDGE_COMPRESSION="${ARANGO_CACHE_MIN_VALUE_SIZE_FOR_EDGE_COMPRESSION:-100}"
export ARANGO_ROCKSDB_AUTO_FILL_INDEX_CACHES_ON_STARTUP="${ARANGO_ROCKSDB_AUTO_FILL_INDEX_CACHES_ON_STARTUP:-true}"
export ARANGO_ROCKSDB_MAX_CONCURRENT_INDEX_FILL_TASKS="${ARANGO_ROCKSDB_MAX_CONCURRENT_INDEX_FILL_TASKS:-8}"
export ARANGO_ROCKSDB_BLOCK_CACHE_SIZE="${ARANGO_ROCKSDB_BLOCK_CACHE_SIZE:-3221225472}" # 3 GiB
export ARANGO_ROCKSDB_WRITE_BUFFER_SIZE="${ARANGO_ROCKSDB_WRITE_BUFFER_SIZE:-536870912}" # 512 MiB
export ARANGO_ROCKSDB_ENFORCE_CACHE_LIMIT="${ARANGO_ROCKSDB_ENFORCE_CACHE_LIMIT:-true}"

echo "[arango-prefill] recreating ArangoDB only; persistent volumes are preserved"

docker compose \
  --env-file storage_layer/.env \
  -p "$PROJECT" \
  -f storage_layer/docker-compose.yml \
  -f storage_layer/docker-compose.arango-bfs-warm.yml \
  up -d --force-recreate --no-deps arangodb

ARGS=()
if [[ -n "$SNAPSHOT" ]]; then
  ARGS+=(--snapshot "$SNAPSHOT")
fi

python3 tools/measurement/wait_arango_prefill.py "${ARGS[@]}"

ARANGO="$(docker ps --format '{{.Names}}' | grep -i arango | head -1)"

echo
echo "[arango-prefill] effective command:"
docker inspect "$ARANGO" --format '{{json .Config.Cmd}}' | jq

echo
echo "[arango-prefill] process state:"
docker stats "$ARANGO" --no-stream
