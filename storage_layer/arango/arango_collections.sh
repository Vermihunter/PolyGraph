#!/usr/bin/env bash
set -euo pipefail

ARANGO_HOST="${ARANGO_HOST:-localhost}"
ARANGO_PORT="${ARANGO_PORT:-8529}"
ARANGO_DB="${ARANGO_DB:-_system}"

ARANGO_USER="${ARANGO_USER:-root}"
ARANGO_PASS="${ARANGO_PASS:-rootpassword}"

BASE_URL="http://${ARANGO_HOST}:${ARANGO_PORT}/_db/${ARANGO_DB}"

AUTH_ARGS=()
if [[ -n "${ARANGO_USER}" ]]; then
  AUTH_ARGS=(-u "${ARANGO_USER}:${ARANGO_PASS}")
fi

read -r -d '' QUERY <<'AQL' || true
LET collections = COLLECTIONS()
RETURN {
  nodeCollections: (
    FOR c IN collections
      FILTER c.type == 2
      RETURN {
        name: c.name,
        type: c.type,
        kind: "node/document",
        isSystem: c.isSystem
      }
  ),
  edgeCollections: (
    FOR c IN collections
      FILTER c.type == 3
      RETURN {
        name: c.name,
        type: c.type,
        kind: "edge",
        isSystem: c.isSystem
      }
  ),
  raw: collections
}
AQL

PAYLOAD=$(jq -n \
  --arg query "$QUERY" \
  '{ query: $query }'
)

curl -sS \
  "${AUTH_ARGS[@]}" \
  -H "Content-Type: application/json" \
  -X POST "${BASE_URL}/_api/cursor" \
  -d "$PAYLOAD" | jq