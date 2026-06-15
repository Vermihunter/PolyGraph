
#!/usr/bin/env bash
set -euo pipefail

NEO4J_HOST="${NEO4J_HOST:-localhost}"
NEO4J_PORT="${NEO4J_PORT:-7474}"
NEO4J_DB="${NEO4J_DB:-neo4j}"

NEO4J_USER="${NEO4J_USER:-neo4j}"
NEO4J_PASS="${NEO4J_PASS:-password}"

LIMIT="${LIMIT:-10}"

BASE_URL="http://${NEO4J_HOST}:${NEO4J_PORT}/db/${NEO4J_DB}/tx/commit"

read -r -d '' QUERY <<'CYPHER' || true
MATCH (n)-[r]->()
RETURN
  elementId(n) AS nodeId,
  labels(n) AS labels,
  properties(n) AS properties,
  count(r) AS outDegree
ORDER BY outDegree DESC
LIMIT $limit
CYPHER

PAYLOAD=$(jq -n \
  --arg query "$QUERY" \
  --argjson limit "$LIMIT" \
  '{
    statements: [
      {
        statement: $query,
        parameters: {
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