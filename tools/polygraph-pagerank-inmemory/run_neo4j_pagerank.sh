#!/usr/bin/env bash
set -Eeuo pipefail

SF="${SF:-1.0}"
REPS="${REPS:-10}"
DAMPING="${DAMPING:-0.85}"
MAX_ITERATIONS="${MAX_ITERATIONS:-20}"
TOLERANCE="${TOLERANCE:-0.0000001}"
GDS_CONCURRENCY="${GDS_CONCURRENCY:-4}"
GDS_GRAPH_NAME="${GDS_GRAPH_NAME:-}"
NEO4J_WARMUPS="${NEO4J_WARMUPS:-1}"

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"

SF_CANON="$(
python3 - "$SF" <<'PY'
from decimal import Decimal
import sys
x=format(Decimal(sys.argv[1]),"f")
if "." not in x: x += ".0"
print(x)
PY
)"
SF_KEY="sf${SF_CANON//./_}"
GDS_GRAPH_NAME="${GDS_GRAPH_NAME:-polygraph_pagerank_${SF_KEY}}"

DIR="data/ldbc-snb/${SF_KEY}/native-neo4j"
DOMAIN="$DIR/domain.json"
BACKENDS="$(jq -r '.paths.backendConfig // .paths.backends // empty' "$DOMAIN")"
if [[ -n "$BACKENDS" && "$BACKENDS" != /* && ! -f "$BACKENDS" ]]; then
    BACKENDS="$(dirname "$DOMAIN")/$BACKENDS"
fi
[[ -f "$BACKENDS" ]] || BACKENDS="$(find "$DIR" -maxdepth 1 \( -name '*backends*.json' -o -name '*backend*.json' \) -print -quit)"

ENDPOINT="$(
 jq -r '.backends[] | select(.kind=="Neo4j" or .id=="Neo4J") | .connection.endpoint // empty' "$BACKENDS" | head -n1
)"
ENDPOINT="${ENDPOINT:-${NEO4J_TX_URL:-http://127.0.0.1:7474/db/neo4j/tx/commit}}"

USER="$(
 jq -r '.backends[] | select(.kind=="Neo4j" or .id=="Neo4J") | .connection.credentials.username // empty' "$BACKENDS" | head -n1
)"
PASSWORD="$(
 jq -r '.backends[] | select(.kind=="Neo4j" or .id=="Neo4J") | .connection.credentials.password // empty' "$BACKENDS" | head -n1
)"
USER="${USER:-${NEO4J_USERNAME:-neo4j}}"
PASSWORD="${PASSWORD:-${NEO4J_PASSWORD:-password}}"

neo() {
    local stmt="$1"
    local params='{}'
    (( $# >= 2 )) && params="$2"
    jq -n --arg statement "$stmt" --argjson params "$params" \
      '{statements:[{statement:$statement,parameters:$params}]}' |
    curl -fsS -u "$USER:$PASSWORD" \
      -H 'Content-Type: application/json' \
      -X POST "$ENDPOINT" -d @-
}

EXISTS="$(
 neo 'CALL gds.graph.exists($name) YIELD exists RETURN exists' \
 "$(jq -n --arg name "$GDS_GRAPH_NAME" '{name:$name}')"
)"
[[ "$(jq -r '.results[0].data[0].row[0] // false' <<<"$EXISTS")" == "true" ]] || {
    echo "Missing GDS graph '$GDS_GRAPH_NAME'." >&2
    echo "Run: SF=$SF_CANON ./prepare_neo4j_pagerank.sh" >&2
    exit 1
}

QUERY='
CALL gds.pageRank.stats($graphName, {
  dampingFactor: $damping,
  maxIterations: toInteger($maxIterations),
  tolerance: $tolerance,
  concurrency: $concurrency,
  logProgress: false
})
YIELD ranIterations,didConverge,preProcessingMillis,computeMillis,postProcessingMillis
RETURN ranIterations,didConverge,preProcessingMillis,computeMillis,postProcessingMillis
'

PARAMS="$(
 jq -n \
   --arg graphName "$GDS_GRAPH_NAME" \
   --argjson damping "$DAMPING" \
   --arg maxIterations "$MAX_ITERATIONS" \
   --argjson tolerance "$TOLERANCE" \
   --argjson concurrency "$GDS_CONCURRENCY" \
   '{graphName:$graphName,damping:$damping,maxIterations:$maxIterations,tolerance:$tolerance,concurrency:$concurrency}'
)"

REQUEST="$(
 jq -n --arg statement "$QUERY" --argjson params "$PARAMS" \
 '{statements:[{statement:$statement,parameters:$params}]}'
)"

once() {
    local rep="$1" print="$2" tmp elapsed
    tmp="$(mktemp)"
    elapsed="$(
      curl -fsS -u "$USER:$PASSWORD" \
        -H 'Content-Type: application/json' \
        -X POST "$ENDPOINT" \
        -d "$REQUEST" \
        -o "$tmp" \
        -w '%{time_total}'
    )"

    if [[ "$(jq '.errors | length' "$tmp")" != "0" ]]; then
        jq '.errors' "$tmp" >&2
        rm -f "$tmp"
        exit 1
    fi

    if [[ "$print" == "1" ]]; then
        jq -r \
          --arg rep "$rep" \
          --arg damping "$DAMPING" \
          --arg maxIterations "$MAX_ITERATIONS" \
          --arg elapsed "$elapsed" \
          '.results[0].data[0].row |
           ["neo4j-gds",$rep,$damping,$maxIterations,.[0],.[1],.[2],.[3],.[4],$elapsed] |
           @csv' "$tmp"
    fi
    rm -f "$tmp"
}

for ((i=0;i<NEO4J_WARMUPS;i++)); do
    once 0 0
done

echo 'backend,rep,damping,max_iterations,ran_iterations,converged,preprocess_ms,compute_ms,postprocess_ms,http_seconds'
for ((i=1;i<=REPS;i++)); do
    once "$i" 1
done
