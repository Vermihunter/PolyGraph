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
REPS="${REPS:-10}"
DAMPING="${DAMPING:-0.85}"
MAX_ITERATIONS="${MAX_ITERATIONS:-20}"
ARANGO_KEEP_JOBS="${ARANGO_KEEP_JOBS:-1}"

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

STATE=".pagerank-state/arango-${SF_KEY}.env"
if [[ -z "${ARANGO_GRAPH_ID:-}" && -f "$STATE" ]]; then
    # shellcheck disable=SC1090
    source "$STATE"
fi

: "${ARANGO_GRAPH_ID:?Set ARANGO_GRAPH_ID or run prepare_arango_pagerank.sh first}"

mkdir -p results/arango

echo 'backend,rep,damping,max_iterations,job_id,job_seconds'

for ((rep=1; rep<=REPS; rep++)); do
    BODY="$(
      jq -n \
        --arg graph "$ARANGO_GRAPH_ID" \
        --argjson damping "$DAMPING" \
        --argjson iterations "$MAX_ITERATIONS" \
        '{
           graph_id:$graph,
           damping_factor:$damping,
           maximum_supersteps:$iterations
         }'
    )"

    start_ns="$(date +%s%N)"
    RESPONSE="$(engine_post "${ARANGO_ENGINE_URL%/}/v1/pagerank" "$BODY")"
    JOB_ID="$(jq -r '.job_id // .jobId // empty' <<<"$RESPONSE")"

    [[ -n "$JOB_ID" ]] || {
        echo "No job_id returned:" >&2
        jq . <<<"$RESPONSE" >&2
        exit 1
    }

    JOB_FILE="results/arango/pagerank-${SF_KEY}-rep${rep}-job${JOB_ID}.json"
    wait_job "$JOB_ID" "$JOB_FILE" >/dev/null
    end_ns="$(date +%s%N)"

    elapsed="$(
      python3 - "$start_ns" "$end_ns" <<'PY'
import sys
print(f"{(int(sys.argv[2])-int(sys.argv[1]))/1e9:.6f}")
PY
    )"

    printf 'arango-gae,%s,%s,%s,%s,%s\n' \
      "$rep" "$DAMPING" "$MAX_ITERATIONS" "$JOB_ID" "$elapsed"

    if [[ "$ARANGO_KEEP_JOBS" != "1" ]]; then
        engine_delete "${ARANGO_ENGINE_URL%/}/v1/jobs/${JOB_ID}" >/dev/null || true
    fi
done
