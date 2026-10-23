#!/usr/bin/env bash
set -Eeuo pipefail

require_arango_engine() {
    : "${ARANGO_ENGINE_URL:?Set ARANGO_ENGINE_URL to the Graph Analytics Engine base URL}"
    : "${ARANGO_TOKEN:?Set ARANGO_TOKEN}"
}

curl_args=(-sS)
if [[ "${CURL_INSECURE:-0}" == "1" ]]; then
    curl_args+=(-k)
fi

auth=(-H "Authorization: bearer ${ARANGO_TOKEN:-}" -H "Content-Type: application/json")

engine_get() {
    curl "${curl_args[@]}" "${auth[@]}" "$1"
}

engine_post() {
    local url="$1"
    local body="$2"
    curl "${curl_args[@]}" "${auth[@]}" -X POST -d "$body" "$url"
}

engine_delete() {
    curl "${curl_args[@]}" "${auth[@]}" -X DELETE "$1"
}

job_is_failure() {
    local json="$1"
    local status
    status="$(jq -r '
      .status //
      .state //
      .current_status.status //
      .job.status //
      .job.state //
      empty
    ' <<<"$json" 2>/dev/null | tr '[:upper:]' '[:lower:]')"

    [[ "$status" =~ fail|error|cancel ]]
}

job_is_success() {
    local json="$1"
    local status terminal done
    status="$(jq -r '
      .status //
      .state //
      .current_status.status //
      .job.status //
      .job.state //
      empty
    ' <<<"$json" 2>/dev/null | tr '[:upper:]' '[:lower:]')"

    terminal="$(jq -r '.is_terminal // .job.is_terminal // false' <<<"$json" 2>/dev/null)"
    done="$(jq -r '.done // .finished // .job.done // .job.finished // false' <<<"$json" 2>/dev/null)"

    if [[ "$status" =~ complete|completed|done|finished|success|succeeded ]]; then
        return 0
    fi
    if [[ "$terminal" == "true" || "$done" == "true" ]]; then
        return 0
    fi

    # Some API revisions expose numeric progress instead of a status string.
    local current total
    current="$(jq -r '.progress.current // .progress_current // empty' <<<"$json" 2>/dev/null)"
    total="$(jq -r '.progress.total // .progress_total // empty' <<<"$json" 2>/dev/null)"
    if [[ -n "$current" && -n "$total" && "$current" == "$total" && "$total" != "0" ]]; then
        return 0
    fi

    return 1
}

wait_job() {
    local job_id="$1"
    local output_file="${2:-}"
    local interval="${JOB_POLL_INTERVAL:-1}"
    local timeout="${JOB_TIMEOUT:-3600}"
    local start now body

    start="$(date +%s)"

    while true; do
        body="$(engine_get "${ARANGO_ENGINE_URL%/}/v1/jobs/${job_id}")"

        if job_is_failure "$body"; then
            [[ -n "$output_file" ]] && printf '%s\n' "$body" >"$output_file"
            echo "Arango Graph Analytics job $job_id failed:" >&2
            jq . <<<"$body" >&2
            return 1
        fi

        if job_is_success "$body"; then
            [[ -n "$output_file" ]] && printf '%s\n' "$body" >"$output_file"
            printf '%s\n' "$body"
            return 0
        fi

        now="$(date +%s)"
        if (( now - start >= timeout )); then
            echo "Timed out waiting for job $job_id after ${timeout}s." >&2
            jq . <<<"$body" >&2
            return 1
        fi

        sleep "$interval"
    done
}


require_arango_engine

SF="${SF:-1.0}"
ARANGO_DATABASE="${ARANGO_DATABASE:-university}"
ARANGO_LOAD_PARALLELISM="${ARANGO_LOAD_PARALLELISM:-4}"
ARANGO_LOAD_BATCH_SIZE="${ARANGO_LOAD_BATCH_SIZE:-400000}"

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

DIR="data/ldbc-snb/${SF_KEY}/native-arango"
DOMAIN="$DIR/domain.json"
[[ -f "$DOMAIN" ]] || {
    echo "Missing $DOMAIN" >&2
    exit 1
}

SCHEMA="$(jq -r '.paths.schema // empty' "$DOMAIN")"
if [[ -n "$SCHEMA" && "$SCHEMA" != /* && ! -f "$SCHEMA" ]]; then
    SCHEMA="$(dirname "$DOMAIN")/$SCHEMA"
fi
[[ -f "$SCHEMA" ]] || SCHEMA="$(find "$DIR" -maxdepth 1 -name '*graph-shape*.json' -print -quit)"
[[ -f "$SCHEMA" ]] || { echo "Cannot find schema." >&2; exit 1; }

VERTICES="$(jq -c '[.nodes[].name] | unique | sort' "$SCHEMA")"
EDGES="$(jq -c '[.relations[].name] | unique | sort' "$SCHEMA")"

BODY="$(
 jq -n \
   --arg db "$ARANGO_DATABASE" \
   --argjson vertices "$VERTICES" \
   --argjson edges "$EDGES" \
   --argjson parallelism "$ARANGO_LOAD_PARALLELISM" \
   --argjson batch "$ARANGO_LOAD_BATCH_SIZE" \
   '{
      database:$db,
      vertex_collections:$vertices,
      edge_collections:$edges,
      vertex_attributes:[],
      parallelism:$parallelism,
      batch_size:$batch
    }'
)"

echo "[arango] Checking Graph Analytics Engine..."
engine_get "${ARANGO_ENGINE_URL%/}/v1/jobs" >/dev/null

echo "[arango] Loading graph topology into engine memory..."
echo "[arango] database:            $ARANGO_DATABASE"
echo "[arango] vertex collections:  $(jq 'length' <<<"$VERTICES")"
echo "[arango] edge collections:    $(jq 'length' <<<"$EDGES")"

start_ns="$(date +%s%N)"
RESPONSE="$(engine_post "${ARANGO_ENGINE_URL%/}/v1/loaddata" "$BODY")"

JOB_ID="$(jq -r '.job_id // .jobId // empty' <<<"$RESPONSE")"
GRAPH_ID="$(jq -r '.graph_id // .graphId // empty' <<<"$RESPONSE")"

[[ -n "$JOB_ID" ]] || {
    echo "No job_id returned:" >&2
    jq . <<<"$RESPONSE" >&2
    exit 1
}
[[ -n "$GRAPH_ID" ]] || {
    echo "No graph_id returned:" >&2
    jq . <<<"$RESPONSE" >&2
    exit 1
}

mkdir -p .pagerank-state results/arango
echo "[arango] load job: $JOB_ID"
echo "[arango] graph id: $GRAPH_ID"

wait_job "$JOB_ID" "results/arango/load-${SF_KEY}-${JOB_ID}.json" >/dev/null

end_ns="$(date +%s%N)"
load_seconds="$(
python3 - "$start_ns" "$end_ns" <<'PY'
import sys
print((int(sys.argv[2])-int(sys.argv[1]))/1e9)
PY
)"

STATE=".pagerank-state/arango-${SF_KEY}.env"
cat >"$STATE" <<EOF
ARANGO_GRAPH_ID='$GRAPH_ID'
ARANGO_ENGINE_URL='${ARANGO_ENGINE_URL}'
ARANGO_DATABASE='${ARANGO_DATABASE}'
EOF

echo "[arango] Graph is resident in Graph Analytics memory."
echo "[arango] graph_id:     $GRAPH_ID"
echo "[arango] load_seconds: $load_seconds"
echo "[arango] state:        $STATE"
