#!/usr/bin/env bash
set -euo pipefail

NEO4J_HOST="${NEO4J_HOST:-localhost}"
NEO4J_PORT="${NEO4J_PORT:-7474}"
NEO4J_DB="${NEO4J_DB:-neo4j}"

NEO4J_USER="${NEO4J_USER:-neo4j}"
NEO4J_PASS="${NEO4J_PASS:-password}"

BASE_URL="http://${NEO4J_HOST}:${NEO4J_PORT}/db/${NEO4J_DB}/tx/commit"

read -r -d '' NODE_QUERY <<'CYPHER' || true
MATCH (n)
UNWIND labels(n) AS label
RETURN label, count(*) AS count
ORDER BY count DESC, label
CYPHER

read -r -d '' EDGE_QUERY <<'CYPHER' || true
MATCH ()-[r]->()
RETURN type(r) AS relationshipType, count(*) AS count
ORDER BY count DESC, relationshipType
CYPHER

PAYLOAD=$(jq -n \
  --arg nodeQuery "$NODE_QUERY" \
  --arg edgeQuery "$EDGE_QUERY" \
  '{
    statements: [
      {
        statement: $nodeQuery,
        resultDataContents: ["row"]
      },
      {
        statement: $edgeQuery,
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