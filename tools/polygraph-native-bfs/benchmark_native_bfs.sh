#!/usr/bin/env bash
set -Eeuo pipefail

BACKEND="${1:?Usage: $0 neo4j|arango}"

SF="${SF:-1.0}"
DEPTH="${DEPTH:-7}"
REPS="${REPS:-5}"
ROOT_LDBC_ID="${ROOT_LDBC_ID:-}"
NEO4J_WARMUPS="${NEO4J_WARMUPS:-1}"
GDS_CONCURRENCY="${GDS_CONCURRENCY:-4}"
GDS_GRAPH_NAME="${GDS_GRAPH_NAME:-}"
GDS_AUTO_PROJECT="${GDS_AUTO_PROJECT:-1}"
ARANGO_WARM_MODE="${ARANGO_WARM_MODE:-none}"
ARANGO_BFS_WARMUPS="${ARANGO_BFS_WARMUPS:-1}"
ARANGO_USE_CACHE="${ARANGO_USE_CACHE:-false}"
ARANGO_QUERY_MEMORY_LIMIT="${ARANGO_QUERY_MEMORY_LIMIT:-8589934592}"
ARANGO_PARALLELISM="${ARANGO_PARALLELISM:-1}"

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"

for cmd in python3 curl jq; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "Missing command: $cmd" >&2
        exit 1
    }
done

SF_CANON="$(
python3 - "$SF" <<'PY'
from decimal import Decimal
import sys
x=format(Decimal(sys.argv[1]),"f")
if "." not in x:
    x += ".0"
print(x)
PY
)"
SF_KEY="sf${SF_CANON//./_}"
GDS_GRAPH_NAME="${GDS_GRAPH_NAME:-polygraph_native_${SF_KEY}}"

case "$BACKEND" in
    neo4j) LAYOUT="native-neo4j" ;;
    arango) LAYOUT="native-arango" ;;
    *) echo "backend must be neo4j or arango" >&2; exit 2 ;;
esac

DIR="data/ldbc-snb/${SF_KEY}/${LAYOUT}"
DOMAIN="$DIR/domain.json"
[[ -f "$DOMAIN" ]] || {
    echo "Missing $DOMAIN" >&2
    exit 1
}

resolve_path() {
    local p="$1"
    if [[ "$p" = /* ]]; then
        echo "$p"
    elif [[ -e "$p" ]]; then
        echo "$p"
    elif [[ -e "$(dirname "$DOMAIN")/$p" ]]; then
        echo "$(dirname "$DOMAIN")/$p"
    else
        echo "$p"
    fi
}

SCHEMA="$(resolve_path "$(jq -r '.paths.schema // empty' "$DOMAIN")")"
BACKENDS="$(resolve_path "$(jq -r '.paths.backendConfig // .paths.backends // empty' "$DOMAIN")")"
[[ -f "$SCHEMA" ]] || SCHEMA="$(find "$DIR" -maxdepth 1 -name '*graph-shape*.json' -print -quit)"
[[ -f "$BACKENDS" ]] || BACKENDS="$(find "$DIR" -maxdepth 1 \( -name '*backends*.json' -o -name '*backend*.json' \) -print -quit)"

if [[ "$BACKEND" == "neo4j" ]]; then
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

    EXISTS="$(
        neo_request \
            'CALL gds.graph.exists($name) YIELD exists RETURN exists' \
            "$(jq -n --arg name "$GDS_GRAPH_NAME" '{name:$name}')"
    )"
    GRAPH_EXISTS="$(jq -r '.results[0].data[0].row[0] // false' <<<"$EXISTS")"

    if [[ "$GRAPH_EXISTS" != "true" ]]; then
        if [[ "$GDS_AUTO_PROJECT" != "1" ]]; then
            echo "GDS graph '$GDS_GRAPH_NAME' is not projected." >&2
            echo "Run: SF=$SF_CANON ./prepare_neo4j_gds.sh" >&2
            exit 3
        fi

        SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
        SF="$SF_CANON" GDS_GRAPH_NAME="$GDS_GRAPH_NAME" \
            "$SCRIPT_DIR/prepare_neo4j_gds.sh" >&2
    fi

    neo_request \
        'CREATE INDEX person_ldbc_id IF NOT EXISTS FOR (p:Person) ON (p.ldbc_id)' \
        >/dev/null

    if [[ -z "$ROOT_LDBC_ID" ]]; then
        ROOT_LDBC_ID="$(
            neo_request \
                'MATCH (p:Person) RETURN min(p.ldbc_id)' |
            jq -r '.results[0].data[0].row[0]'
        )"
    fi

    CYPHER='
MATCH (start:Person {ldbc_id:$root})
WITH id(start) AS sourceId
CALL gds.bfs.stream($graphName, {
  sourceNode: sourceId,
  maxDepth: toInteger($depth),
  concurrency: $concurrency,
  logProgress: false
})
YIELD nodeIds
RETURN size(nodeIds) AS reached
'

    REQUEST="$(
        jq -n \
            --arg statement "$CYPHER" \
            --argjson root "$ROOT_LDBC_ID" \
            --arg depth "$DEPTH" \
            --argjson concurrency "$GDS_CONCURRENCY" \
            --arg graphName "$GDS_GRAPH_NAME" \
            '{
              statements:[{
                statement:$statement,
                parameters:{
                  root:$root,
                  depth:$depth,
                  concurrency:$concurrency,
                  graphName:$graphName
                }
              }]
            }'
    )"

    neo_once() {
        local rep="$1"
        local print="$2"
        local tmp elapsed reached
        tmp="$(mktemp)"

        elapsed="$(
            curl -fsS \
                -u "${USER}:${PASSWORD}" \
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

        reached="$(jq -r '.results[0].data[0].row[0]' "$tmp")"
        rm -f "$tmp"

        if [[ "$print" == "1" ]]; then
            printf 'neo4j,gds-memory,%s,%s,%s,%s,%s\n' \
                "$rep" "$ROOT_LDBC_ID" "$DEPTH" "$reached" "$elapsed"
        fi
    }

    echo "[neo4j] GDS graph:    $GDS_GRAPH_NAME" >&2
    echo "[neo4j] root:         $ROOT_LDBC_ID" >&2
    echo "[neo4j] max depth:    $DEPTH" >&2
    echo "[neo4j] concurrency:  $GDS_CONCURRENCY" >&2
    echo "[neo4j] warmups:      $NEO4J_WARMUPS" >&2

    for ((i=0; i<NEO4J_WARMUPS; ++i)); do
        neo_once 0 0
    done

    echo 'backend,cache_mode,rep,root_ldbc_id,max_depth,reached,curl_seconds'
    for ((i=1; i<=REPS; ++i)); do
        neo_once "$i" 1
    done
    exit 0
fi


case "$ARANGO_WARM_MODE" in none|bfs|full-scan) ;; *) echo 'ARANGO_WARM_MODE must be none, bfs, or full-scan' >&2; exit 2;; esac
DB="$(jq -r '.backends[] | select(.kind == "ArangoDB" or .id == "ArangoDB") | .connection.database // empty' "$BACKENDS" | head -n1)"; DB="${DB:-${ARANGO_DATABASE:-university}}"; USER="${ARANGO_USER:-root}"; PASSWORD="${ARANGO_ROOT_PASSWORD:-rootpassword}"; BASE="${ARANGO_URL:-http://127.0.0.1:8529}"; CURSOR="$BASE/_db/$DB/_api/cursor"
arango_query(){
  local q="$1"
  local vars='{}'
  if (( $# >= 2 )); then vars="$2"; fi
  jq -n --arg query "$q" --argjson vars "$vars"     '{query:$query,bindVars:$vars}' |
  curl -fsS -u "$USER:$PASSWORD"     -H 'Content-Type: application/json'     -X POST "$CURSOR" -d @-
}
[[ -n "$ROOT_LDBC_ID" ]] || ROOT_LDBC_ID="$(arango_query 'FOR p IN Person SORT p.ldbc_id ASC LIMIT 1 RETURN p.ldbc_id' | jq -r '.result[0]')"
START_ID="$(arango_query 'FOR p IN Person FILTER p.ldbc_id == @root LIMIT 1 RETURN p._id' "$(jq -n --argjson root "$ROOT_LDBC_ID" '{root:$root}')" | jq -r '.result[0]')"
[[ -n "$START_ID" && "$START_ID" != null ]] || { echo "Person $ROOT_LDBC_ID not found" >&2; exit 1; }
EDGE_COLLECTIONS="$(python3 - "$SCHEMA" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
print(', '.join(f'`{x}`' for x in sorted({r['name'] for r in d['relations']})))
PY
)"

case "$ARANGO_USE_CACHE" in
    true|false) ;;
    *)
        echo "ARANGO_USE_CACHE must be true or false" >&2
        exit 2
        ;;
esac

[[ "$ARANGO_QUERY_MEMORY_LIMIT" =~ ^[0-9]+$ ]] || {
    echo "ARANGO_QUERY_MEMORY_LIMIT must be an integer byte count" >&2
    exit 2
}
[[ "$ARANGO_PARALLELISM" =~ ^[0-9]+$ ]] || {
    echo "ARANGO_PARALLELISM must be an integer" >&2
    exit 2
}

# Memory-conscious native BFS:
#
# - only emit the vertex variable; edge/path objects are not requested
# - global vertex uniqueness implements normal BFS visited-set semantics
# - edge uniqueness is unnecessary when vertices are globally unique
# - parallelism=1 avoids multiplying traversal state for one start vertex
# - useCache=false by default so a whole-graph traversal does not flood
#   ArangoDB's in-memory edge cache
AQL="
FOR v IN 0..${DEPTH} ANY @startId ${EDGE_COLLECTIONS}
  OPTIONS {
    order: \"bfs\",
    uniqueVertices: \"global\",
    uniqueEdges: \"none\",
    parallelism: ${ARANGO_PARALLELISM},
    useCache: ${ARANGO_USE_CACHE}
  }
  COLLECT WITH COUNT INTO reached
  RETURN reached
"

REQUEST="$(
    jq -n \
        --arg query "$AQL" \
        --arg startId "$START_ID" \
        --argjson memoryLimit "$ARANGO_QUERY_MEMORY_LIMIT" \
        '{
          query:$query,
          bindVars:{startId:$startId},
          memoryLimit:$memoryLimit,
          options:{cache:false}
        }'
)"

diagnose_arango_failure() {
    echo >&2
    echo "================ Arango failure diagnostics ================" >&2
    free -h >&2 2>/dev/null || true

    if command -v docker >/dev/null 2>&1; then
        local container
        container="$(
            docker ps -a --format '{{.Names}}' 2>/dev/null |
            grep -E "polygraph-${SF_KEY//_/-}-schema-based-arangodb-1|schema-based-arangodb" |
            head -n1 || true
        )"

        if [[ -n "$container" ]]; then
            echo >&2
            echo "container: $container" >&2
            docker inspect "$container" \
                --format 'status={{.State.Status}} oomKilled={{.State.OOMKilled}} exitCode={{.State.ExitCode}} restartCount={{.RestartCount}} memoryLimit={{.HostConfig.Memory}}' \
                >&2 2>/dev/null || true
            echo >&2
            echo "--- last Arango logs ---" >&2
            docker logs --tail 100 "$container" >&2 2>/dev/null || true
        fi
    fi

    echo "============================================================" >&2
}

arango_once() {
    local rep="$1"
    local print="$2"
    local tmp elapsed reached rc

    tmp="$(mktemp)"

    set +e
    elapsed="$(
        curl -sS \
            -u "$USER:$PASSWORD" \
            -H 'Content-Type: application/json' \
            -X POST "$CURSOR" \
            -d "$REQUEST" \
            -o "$tmp" \
            -w '%{time_total}'
    )"
    rc=$?
    set -e

    if (( rc != 0 )); then
        echo "ArangoDB request failed (curl exit code $rc)." >&2
        [[ -s "$tmp" ]] && cat "$tmp" >&2
        rm -f "$tmp"
        diagnose_arango_failure
        exit "$rc"
    fi

    if [[ "$(jq -r '.error // false' "$tmp" 2>/dev/null)" == "true" ]]; then
        jq . "$tmp" >&2
        rm -f "$tmp"
        exit 1
    fi

    reached="$(jq -r '.result[0]' "$tmp")"
    rm -f "$tmp"

    if [[ "$print" == "1" ]]; then
        printf 'arango,%s,%s,%s,%s,%s,%s\n' \
            "$ARANGO_WARM_MODE" "$rep" "$ROOT_LDBC_ID" \
            "$DEPTH" "$reached" "$elapsed"
    fi
}

echo "[arango] root:                $ROOT_LDBC_ID" >&2
echo "[arango] max depth:           $DEPTH" >&2
echo "[arango] edge cache:          $ARANGO_USE_CACHE" >&2
echo "[arango] traversal threads:   $ARANGO_PARALLELISM" >&2
echo "[arango] query memory limit:  $ARANGO_QUERY_MEMORY_LIMIT bytes" >&2
echo "[arango] warm mode:           $ARANGO_WARM_MODE" >&2

if [[ "$ARANGO_WARM_MODE" == full-scan ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    SF="$SF_CANON" "$SCRIPT_DIR/warm_arango.sh" >&2
fi

if [[ "$ARANGO_WARM_MODE" == bfs || "$ARANGO_WARM_MODE" == full-scan ]]; then
    for ((i=0;i<ARANGO_BFS_WARMUPS;i++)); do
        arango_once 0 0
    done
fi

echo 'backend,cache_mode,rep,root_ldbc_id,max_depth,reached,curl_seconds'
for ((i=1;i<=REPS;i++)); do
    arango_once "$i" 1
done
