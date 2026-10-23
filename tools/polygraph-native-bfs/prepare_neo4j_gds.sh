#!/usr/bin/env bash
set -Eeuo pipefail

SF="${SF:-1.0}"
GDS_GRAPH_NAME="${GDS_GRAPH_NAME:-}"
GDS_PROJECT_CONCURRENCY="${GDS_PROJECT_CONCURRENCY:-4}"
GDS_REPROJECT="${GDS_REPROJECT:-0}"

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"

for cmd in python3 jq curl; do
    command -v "$cmd" >/dev/null 2>&1 || {
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
GDS_GRAPH_NAME="${GDS_GRAPH_NAME:-polygraph_native_${SF_KEY}}"

DIR="data/ldbc-snb/${SF_KEY}/native-neo4j"
DOMAIN="${DIR}/domain.json"
[[ -f "$DOMAIN" ]] || {
    echo "Missing $DOMAIN" >&2
    echo "Run prepare_native_storage.sh neo4j first." >&2
    exit 1
}

resolve_path() {
    local p="$1"
    [[ -z "$p" ]] && return 1
    if [[ "$p" = /* && -f "$p" ]]; then
        printf '%s\n' "$p"
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

[[ -f "$SCHEMA" ]] || { echo "Cannot find graph schema." >&2; exit 1; }
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

neo_request() {
    local statement="$1"
    local params='{}'
    if (( $# >= 2 )); then
        params="$2"
    fi

    jq -n \
        --arg statement "$statement" \
        --argjson params "$params" \
        '{statements:[{statement:$statement,parameters:$params}]}' |
    curl -fsS \
        -u "${USER}:${PASSWORD}" \
        -H 'Content-Type: application/json' \
        -X POST "$ENDPOINT" \
        -d @-
}

echo "[gds] checking GDS procedures..."

CHECK="$(
    neo_request '
SHOW PROCEDURES YIELD name
WHERE name IN ["gds.graph.project", "gds.bfs.stream"]
RETURN collect(name)
'
)"

if [[ "$(jq '.errors | length' <<<"$CHECK")" != "0" ]]; then
    jq '.errors' <<<"$CHECK" >&2
    exit 1
fi

FOUND="$(jq -c '.results[0].data[0].row[0] // []' <<<"$CHECK")"
echo "[gds] procedures: $FOUND"

if ! jq -e '.results[0].data[0].row[0] | index("gds.graph.project") != null' <<<"$CHECK" >/dev/null; then
    echo "gds.graph.project is unavailable." >&2
    exit 1
fi
if ! jq -e '.results[0].data[0].row[0] | index("gds.bfs.stream") != null' <<<"$CHECK" >/dev/null; then
    echo "gds.bfs.stream is unavailable in this GDS installation." >&2
    exit 1
fi

EXISTS="$(
    neo_request \
        'CALL gds.graph.exists($name) YIELD exists RETURN exists' \
        "$(jq -n --arg name "$GDS_GRAPH_NAME" '{name:$name}')"
)"
GRAPH_EXISTS="$(jq -r '.results[0].data[0].row[0] // false' <<<"$EXISTS")"

if [[ "$GRAPH_EXISTS" == "true" && "$GDS_REPROJECT" != "1" ]]; then
    echo "[gds] '$GDS_GRAPH_NAME' is already in memory; reusing it."
    neo_request \
        'CALL gds.graph.list($name)
         YIELD graphName,nodeCount,relationshipCount,memoryUsage
         RETURN graphName,nodeCount,relationshipCount,memoryUsage' \
        "$(jq -n --arg name "$GDS_GRAPH_NAME" '{name:$name}')" |
        jq -r '.results[0].data[]?.row | @tsv'
    exit 0
fi

if [[ "$GRAPH_EXISTS" == "true" ]]; then
    echo "[gds] dropping existing graph '$GDS_GRAPH_NAME'..."
    neo_request \
        'CALL gds.graph.drop($name) YIELD graphName RETURN graphName' \
        "$(jq -n --arg name "$GDS_GRAPH_NAME" '{name:$name}')" \
        >/dev/null
fi

# Build a relationship projection map from the exact LDBC schema.
# Every stored relationship type is projected UNDIRECTED so its semantics
# match the Arango ANY traversal and the old APOC both-direction traversal.
REL_PROJECTION="$(
python3 - "$SCHEMA" <<'PY'
import json, sys
schema = json.load(open(sys.argv[1], encoding="utf-8"))
types = sorted({r["name"] for r in schema["relations"]})
print(json.dumps({
    t: {"type": t, "orientation": "UNDIRECTED"}
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

echo "[gds] projecting the complete Neo4j graph into memory..."
echo "[gds] graph name:   $GDS_GRAPH_NAME"
echo "[gds] relationship types: $(jq 'length' <<<"$REL_PROJECTION")"
echo "[gds] orientation:  UNDIRECTED"
echo "[gds] concurrency:  $GDS_PROJECT_CONCURRENCY"

RESULT="$(
    neo_request '
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

echo "[gds] projection ready:"
jq -r '
.results[0].data[0].row
| "      graph=\(.[0]) nodes=\(.[1]) relationships=\(.[2]) project_ms=\(.[3])"
' <<<"$RESULT"
