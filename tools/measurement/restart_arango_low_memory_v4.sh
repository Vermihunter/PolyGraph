#!/usr/bin/env bash
set -euo pipefail
PROJECT="${1:-polygraph-sf1-0-schema-based}"

export ARANGO_ROCKSDB_BLOCK_CACHE_SIZE="${ARANGO_ROCKSDB_BLOCK_CACHE_SIZE:-3221225472}"
export ARANGO_CACHE_SIZE="${ARANGO_CACHE_SIZE:-2147483648}"
export ARANGO_CACHE_MIN_VALUE_SIZE_FOR_EDGE_COMPRESSION="${ARANGO_CACHE_MIN_VALUE_SIZE_FOR_EDGE_COMPRESSION:-100}"
export ARANGO_ROCKSDB_WRITE_BUFFER_SIZE="${ARANGO_ROCKSDB_WRITE_BUFFER_SIZE:-268435456}"
export ARANGO_ROCKSDB_ENFORCE_CACHE_LIMIT="${ARANGO_ROCKSDB_ENFORCE_CACHE_LIMIT:-true}"

docker compose   --env-file storage_layer/.env   -p "$PROJECT"   -f storage_layer/docker-compose.yml   -f storage_layer/docker-compose.low-memory-v4.yml   up -d --force-recreate --no-deps arangodb

ARANGO="$(docker ps --format '{{.Names}}' | grep -i arango | head -1)"
echo
docker inspect "$ARANGO" --format '{{json .Config.Cmd}}' | jq
