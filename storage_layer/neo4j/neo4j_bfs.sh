#!/usr/bin/env bash
set -euo pipefail

NEO4J_HOST="${NEO4J_HOST:-localhost}"
NEO4J_PORT="${NEO4J_PORT:-7474}"
NEO4J_DB="${NEO4J_DB:-neo4j}"

NEO4J_USER="${NEO4J_USER:-neo4j}"
NEO4J_PASS="${NEO4J_PASS:-password}"

START_ELEMENT_ID="${START_ELEMENT_ID:-}"
START_ID="${START_ID:-1}"
START_NAME="${START_NAME:-}"

MAX_DEPTH="${MAX_DEPTH:-3}"
LIMIT="${LIMIT:-100}"
DIRECTION="${DIRECTION:-OUT}"

BASE_URL="http://${NEO4J_HOST}:${NEO4J_PORT}/db/${NEO4J_DB}/tx/commit"

if ! [[ "$MAX_DEPTH" =~ ^[0-9]+$ ]]; then
  echo "MAX_DEPTH must be an integer"
  exit 1
fi

if ! [[ "$LIMIT" =~ ^[0-9]+$ ]]; then
  echo "LIMIT must be an integer"
  exit 1
fi

case "$DIRECTION" in
  OUT)
    PATTERN="-[*0..${MAX_DEPTH}]->"
    ;;
  IN)
    PATTERN="<-[*0..${MAX_DEPTH}]-"
    ;;
  ANY)
    PATTERN="-[*0..${MAX_DEPTH}]-"
    ;;
  *)
    echo "DIRECTION must be OUT, IN, or ANY"
    exit 1
    ;;
esac

read -r -d '' QUERY <<CYPHER || true
MATCH (start)
WHERE
  (\$startElementId <> "" AND elementId(start) = \$startElementId)
  OR
  (\$startId <> "" AND toString(start.id) = \$startId)
  OR
  (\$startName <> "" AND toString(start.name) = \$startName)
WITH start
LIMIT 1

MATCH p = (start)${PATTERN}(v)
WITH v, p
ORDER BY length(p) ASC

WITH v, collect(p)[0] AS shortestPath
RETURN
  length(shortestPath) AS depth,
  elementId(v) AS nodeId,
  labels(v) AS labels,
  properties(v) AS properties,
  [n IN nodes(shortestPath) | {
    id: elementId(n),
    labels: labels(n),
    properties: properties(n)
  }] AS path
ORDER BY depth ASC, nodeId
LIMIT \$limit
CYPHER

PAYLOAD=$(jq -n \
  --arg query "$QUERY" \
  --arg startElementId "$START_ELEMENT_ID" \
  --arg startId "$START_ID" \
  --arg startName "$START_NAME" \
  --argjson limit "$LIMIT" \
  '{
    statements: [
      {
        statement: $query,
        parameters: {
          startElementId: $startElementId,
          startId: $startId,
          startName: $startName,
          limit: $limit
        },
        resultDataContents: ["row"]
      }
    ]
  }'
)

curl -sS \
  -u "${NEO4J_USER}:${NEO4J_PASS}" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json" \
  -X POST "$BASE_URL" \
  -d "$PAYLOAD" | jq