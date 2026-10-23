#!/usr/bin/env bash
set -Eeuo pipefail

SF="${SF:-1.0}"
GDS_GRAPH_NAME="${GDS_GRAPH_NAME:-}"
GDS_PROJECT_CONCURRENCY="${GDS_PROJECT_CONCURRENCY:-4}"
GDS_REPROJECT="${GDS_REPROJECT:-0}"

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"

for cmd in python3 jq curl; do
    command -v "$cmd" >/dev/null || {
        echo "Missing command: $cmd" >&2
        exit 1
    }
done

SF_CANON="$(
python3 - "$SF" <<'PY'
from decimal import Decimal
import sys
x = format(Decimal(sys.argv[1]), "f")
if "." not in x:
    x += ".0"
print(x)
PY
)"
SF_KEY="sf${SF_CANON//./_}"
GDS_GRAPH_NAME="${GDS_GRAPH_NAME:-polygraph_pagerank_${SF_KEY}}"

DIR="data/ldbc-snb/${SF_KEY}/native-neo4j"
DOMAIN="${DIR}/domain.json"

[[ -f "$DOMAIN" ]] || {
    echo "Missing $DOMAIN" >&2
    exit 1
}

resolve_path() {
    local p="$1"
    [[ -z "$p" ]] && return 1
    if [[ "$p" = /* && -f "$p" ]]; then
        echo "$p"
    elif [[ -f "$p" ]]; then
        realpath "$p"
    elif [[ -f "$(dirname "$DOMAIN")/$p" ]]; then
        realpath "$(dirname "$DOMAIN")/$p"
    else
        return 1
    fi
}

SCHEMA="$(resolve_path "$(jq -r '.paths.schema // empty' "$DOMAIN")" || true)"
BACKENDS="$(resolve_path "$(jq -r '.paths.backendConfig // .paths.backends // empty' "$DOMAIN")" || true)"

[[ -f "$SCHEMA" ]] || SCHEMA="$(find "$DIR" -maxdepth 1 -name '*graph-shape*.json' -print -quit)"
[[ -f "$BACKENDS" ]] || BACKENDS="$(find "$DIR" -maxdepth 1 \( -name '*backends*.json' -o -name '*backend*.json' \) -print -quit)"

[[ -f "$SCHEMA" ]] || { echo "Cannot find schema." >&2; exit 1; }
[[ -f "$BACKENDS" ]] || { echo "Cannot find backend config." >&2; exit 1; }

ENDPOINT="$(
    jq -r '
      .backends[]
      | select(.kind == "Neo4j" or .id == "Neo4J")
      | .connection.endpoint // empty
    ' "$BACKENDS" | head -n1
)"
ENDPOINT="${ENDPOINT:-${NEO4J_TX_URL:-http://127.0.0.1:7474/db/neo4j/tx/commit}}"

USER="$(
    jq -r '
      .backends[]
      | select(.kind == "Neo4j" or .id == "Neo4J")
      | .connection.credentials.username // empty
    ' "$BACKENDS" | head -n1
)"
PASSWORD="$(
    jq -r '
      .backends[]
      | select(.kind == "Neo4j" or .id == "Neo4J")
      | .connection.credentials.password // empty
    ' "$BACKENDS" | head -n1
)"

USER="${USER:-${NEO4J_USERNAME:-neo4j}}"
PASSWORD="${PASSWORD:-${NEO4J_PASSWORD:-password}}"

neo() {
    local stmt="$1"
    local params='{}'
    (( $# >= 2 )) && params="$2"

    jq -n \
      --arg statement "$stmt" \
      --argjson params "$params" \
      '{statements:[{statement:$statement,parameters:$params}]}' |
    curl -fsS \
      -u "$USER:$PASSWORD" \
      -H 'Content-Type: application/json' \
      -X POST "$ENDPOINT" \
      -d @-
}

CHECK="$(neo '
SHOW PROCEDURES YIELD name
WHERE name IN ["gds.graph.project","gds.pageRank.stats"]
RETURN collect(name)
')"

[[ "$(jq '.errors | length' <<<"$CHECK")" == "0" ]] || {
    jq '.errors' <<<"$CHECK" >&2
    exit 1
}

echo "[neo4j] procedures: $(jq -c '.results[0].data[0].row[0]' <<<"$CHECK")"

EXISTS="$(
    neo 'CALL gds.graph.exists($name) YIELD exists RETURN exists' \
        "$(jq -n --arg name "$GDS_GRAPH_NAME" '{name:$name}')"
)"
HAS_GRAPH="$(jq -r '.results[0].data[0].row[0] // false' <<<"$EXISTS")"

if [[ "$HAS_GRAPH" == "true" && "$GDS_REPROJECT" != "1" ]]; then
    echo "[neo4j] Reusing GDS graph: $GDS_GRAPH_NAME"
    neo '
CALL gds.graph.list($name)
YIELD graphName,nodeCount,relationshipCount,memoryUsage
RETURN graphName,nodeCount,relationshipCount,memoryUsage
' "$(jq -n --arg name "$GDS_GRAPH_NAME" '{name:$name}')" |
        jq -r '.results[0].data[]?.row | @tsv'
    exit 0
fi

if [[ "$HAS_GRAPH" == "true" ]]; then
    echo "[neo4j] Dropping old projection..."
    neo 'CALL gds.graph.drop($name) YIELD graphName RETURN graphName' \
        "$(jq -n --arg name "$GDS_GRAPH_NAME" '{name:$name}')" \
        >/dev/null
fi

REL_PROJECTION="$(
python3 - "$SCHEMA" <<'PY'
import json, sys
schema = json.load(open(sys.argv[1], encoding="utf-8"))
types = sorted({r["name"] for r in schema["relations"]})
print(json.dumps({
    t: {"type": t, "orientation": "NATURAL"}
    for t in types
}, separators=(",", ":")))
PY
)"

PARAMS="$(
    jq -n \
      --arg name "$GDS_GRAPH_NAME" \
      --argjson rels "$REL_PROJECTION" \
      --argjson concurrency "$GDS_PROJECT_CONCURRENCY" \
      '{name:$name,rels:$rels,concurrency:$concurrency}'
)"

echo "[neo4j] Projecting directed graph into GDS memory..."
echo "[neo4j] name:          $GDS_GRAPH_NAME"
echo "[neo4j] orientation:   NATURAL"
echo "[neo4j] relation types: $(jq 'length' <<<"$REL_PROJECTION")"

RESULT="$(
neo '
CALL gds.graph.project(
  $name,
  "*",
  $rels,
  {readConcurrency:$concurrency}
)
YIELD graphName,nodeCount,relationshipCount,projectMillis
RETURN graphName,nodeCount,relationshipCount,projectMillis
' "$PARAMS"
)"

if [[ "$(jq '.errors | length' <<<"$RESULT")" != "0" ]]; then
    jq '.errors' <<<"$RESULT" >&2
    exit 1
fi

jq -r '
.results[0].data[0].row
| "[neo4j] graph=\(.[0]) nodes=\(.[1]) relationships=\(.[2]) project_ms=\(.[3])"
' <<<"$RESULT"
