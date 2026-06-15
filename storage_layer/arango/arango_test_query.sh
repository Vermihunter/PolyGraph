#!/usr/bin/env bash
set -euo pipefail

ARANGO_HOST="${ARANGO_HOST:-localhost}"
ARANGO_PORT="${ARANGO_PORT:-8529}"
ARANGO_DB="${ARANGO_DB:-_system}"

ARANGO_USER="${ARANGO_USER:-root}"
ARANGO_PASS="${ARANGO_PASS:-rootpassword}"

NODE_COLLECTION="${NODE_COLLECTION:-vertices}"
EDGE_COLLECTION="${EDGE_COLLECTION:-edges}"
LIMIT="${LIMIT:-10}"

BASE_URL="http://${ARANGO_HOST}:${ARANGO_PORT}/_db/${ARANGO_DB}"

AUTH_ARGS=()
if [[ -n "${ARANGO_USER}" ]]; then
  AUTH_ARGS=(-u "${ARANGO_USER}:${ARANGO_PASS}")
fi

read -r -d '' QUERY <<'AQL' || true
FOR v IN @@nodeCollection
  LET outDegree = LENGTH(
    FOR e IN @@edgeCollection
      FILTER e._from == v._id
      RETURN 1
  )
  SORT outDegree DESC
  LIMIT @limit
  RETURN {
    vertex: v._id,
    key: v._key,
    outDegree
  }
AQL

PAYLOAD=$(jq -n \
  --arg query "$QUERY" \
  --arg nodeCollection "$NODE_COLLECTION" \
  --arg edgeCollection "$EDGE_COLLECTION" \
  --argjson limit "$LIMIT" \
  '{
    query: $query,
    bindVars: {
      "@nodeCollection": $nodeCollection,
      "@edgeCollection": $edgeCollection,
      limit: $limit
    }
  }'
)

curl -sS \
  "${AUTH_ARGS[@]}" \
  -H "Content-Type: application/json" \
  -X POST "${BASE_URL}/_api/cursor" \
  -d "$PAYLOAD" | jq