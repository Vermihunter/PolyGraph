#!/usr/bin/env bash
set -euo pipefail

ARANGO_HOST="${ARANGO_HOST:-localhost}"
ARANGO_PORT="${ARANGO_PORT:-8529}"
ARANGO_DB="${ARANGO_DB:-_system}"

ARANGO_USER="${ARANGO_USER:-root}"
ARANGO_PASS="${ARANGO_PASS:-rootpassword}"

START_VERTEX="${START_VERTEX:-vertices/1}"
EDGE_COLLECTION="${EDGE_COLLECTION:-edges}"
MAX_DEPTH="${MAX_DEPTH:-3}"
LIMIT="${LIMIT:-100}"
DIRECTION="${DIRECTION:-OUTBOUND}"

BASE_URL="http://${ARANGO_HOST}:${ARANGO_PORT}/_db/${ARANGO_DB}"

AUTH_ARGS=()
if [[ -n "${ARANGO_USER}" ]]; then
  AUTH_ARGS=(-u "${ARANGO_USER}:${ARANGO_PASS}")
fi

if ! [[ "$MAX_DEPTH" =~ ^[0-9]+$ ]]; then
  echo "MAX_DEPTH must be an integer"
  exit 1
fi

if ! [[ "$LIMIT" =~ ^[0-9]+$ ]]; then
  echo "LIMIT must be an integer"
  exit 1
fi

case "$DIRECTION" in
  OUTBOUND|INBOUND|ANY) ;;
  *)
    echo "DIRECTION must be OUTBOUND, INBOUND, or ANY"
    exit 1
    ;;
esac

read -r -d '' QUERY <<AQL || true
FOR v, e, p IN 0..@maxDepth ${DIRECTION} @startVertex @@edgeCollection
  OPTIONS {
    order: "bfs",
    uniqueVertices: "global"
  }
  LIMIT @limit
  RETURN {
    depth: LENGTH(p.edges),
    vertex: v._id,
    key: v._key,
    path: p.vertices[*]._id,
    edges: p.edges[*]._id
  }
AQL

PAYLOAD=$(jq -n \
  --arg query "$QUERY" \
  --arg startVertex "$START_VERTEX" \
  --arg edgeCollection "$EDGE_COLLECTION" \
  --argjson maxDepth "$MAX_DEPTH" \
  --argjson limit "$LIMIT" \
  '{
    query: $query,
    bindVars: {
      startVertex: $startVertex,
      "@edgeCollection": $edgeCollection,
      maxDepth: $maxDepth,
      limit: $limit
    }
  }'
)

curl -sS \
  "${AUTH_ARGS[@]}" \
  -H "Content-Type: application/json" \
  -X POST "${BASE_URL}/_api/cursor" \
  -d "$PAYLOAD" | jq