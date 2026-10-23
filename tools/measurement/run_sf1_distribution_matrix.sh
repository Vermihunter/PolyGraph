#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# Reproduce the SF1 Arango no-cache sweep with a bounded 16-thread scheduler.
# Exported values override Compose .env interpolation for newly created containers.
export ARANGO_SERVER_MINIMAL_THREADS="${ARANGO_SERVER_MINIMAL_THREADS:-16}"
export ARANGO_SERVER_MAXIMAL_THREADS="${ARANGO_SERVER_MAXIMAL_THREADS:-16}"

PYTHON_BIN="${PYTHON_BIN:-python3}"
TIMING_REPETITIONS="${1:-${TIMING_REPETITIONS:-5}}"
ANALYSIS_REPETITIONS="${ANALYSIS_REPETITIONS:-1}"
ROOT_COUNT="${ROOT_COUNT:-4}"
MAX_DEPTH="${MAX_DEPTH:-7}"
DIRECTION="${DIRECTION:-both}"
HTTP_PORT="${HTTP_PORT:-8080}"
HTTP_TIMEOUT="${HTTP_TIMEOUT:-900}"
RESTART_STORAGE_PER_CASE="${RESTART_STORAGE_PER_CASE:-1}"
RUN_ANALYSIS="${RUN_ANALYSIS:-1}"
RUN_TIMING="${RUN_TIMING:-1}"
OUT="${OUT:-measurements/sf1-distribution-matrix-$(date +%Y%m%d-%H%M%S)}"

DISTRIBUTIONS=(
  schema-based
  hash-attributes
  label-striped-3
  schema-family
  proxy-minimized
)

# Run the complete cache-policy matrix for every persistent distribution.
CACHE_NAMES=(
  01-gpu-100
  02-redis-100
  03-mmap-100
  04-gpu30-redis20-mmap20
  05-gpu20-redis15-mmap15
  06-redis-25
  07-mmap-25
  08-no-cache
)

NORMAL_SERVER="${NORMAL_SERVER:-build/http-server/bin/polygraph_http_server}"
ANALYZER_SERVER="${ANALYZER_SERVER:-build/http-server-analysis/bin/polygraph_http_server}"

mkdir -p "$OUT" "$OUT/_requests" "$OUT/_logs"
STATUS="$OUT/status.tsv"
printf 'phase\tdistribution\tcache\tstatus\texit_code\tfinished\n' > "$STATUS"

log() { printf '\n============================================================\n%s\n============================================================\n' "$*"; }

project_for() { printf 'polygraph-sf1-0-%s' "$1"; }

stop_project() {
  local project="$1"
  docker compose \
    --env-file storage_layer/.env \
    -f storage_layer/docker-compose.yml \
    --project-name "$project" \
    stop >/dev/null 2>&1 || true
}

stop_all_matrix_projects() {
  local distribution
  for distribution in "${DISTRIBUTIONS[@]}"; do
    stop_project "$(project_for "$distribution")"
  done
}

stop_sf10_schema_project() {
  stop_project polygraph-sf10-0-schema-based
}

ensure_redis() {
  local command=(docker compose)
  if [[ -f storage_layer/redis/.env.sf1-benchmark ]]; then
    command+=(--env-file storage_layer/redis/.env.sf1-benchmark)
  fi
  command+=(-f storage_layer/redis/docker-compose.yml up -d --wait)
  "${command[@]}"
}

wait_project() {
  local project="$1"
  docker compose \
    --env-file storage_layer/.env \
    -f storage_layer/docker-compose.yml \
    --project-name "$project" \
    up -d --wait neo4j arangodb postgresql
}

restart_project() {
  local project="$1"
  docker compose \
    --env-file storage_layer/.env \
    -f storage_layer/docker-compose.yml \
    --project-name "$project" \
    restart neo4j arangodb postgresql >/dev/null
  wait_project "$project"
}

activate_distribution() {
  local distribution="$1"
  "$PYTHON_BIN" tools/polygraph.py instance 1 "$distribution" activate
  wait_project "$(project_for "$distribution")"
}

generate_request_plan() {
  local distribution="$1"
  local request_plan="$OUT/_requests/$distribution.json"
  "$PYTHON_BIN" tools/measurement/generate_sf1_distribution_requests.py \
    --roots "data/ldbc-snb/sf1_0/$distribution/bfs_roots.csv" \
    --root-count "$ROOT_COUNT" \
    --max-depth "$MAX_DEPTH" \
    --direction "$DIRECTION" \
    --output "$request_plan" >/dev/null
  printf '%s' "$request_plan"
}

run_case() {
  local phase="$1"
  local distribution="$2"
  local cache="$3"
  local repetitions="$4"
  local server="$5"
  local domain="data/ldbc-snb/sf1_0/$distribution/domain.json"
  local plan="data/ldbc-snb/sf1_0/$distribution/caches/$cache/cache-plan.json"
  local request_plan="$OUT/_requests/$distribution.json"
  local case_out="$OUT/$phase/$distribution/$cache"
  local log_file="$OUT/_logs/$phase-$distribution-$cache.log"
  local project="$(project_for "$distribution")"

  mkdir -p "$case_out"
  if [[ ! -f "$domain" || ! -f "$plan" ]]; then
    echo "Missing domain/cache plan for $distribution/$cache" | tee "$log_file"
    printf '%s\t%s\t%s\tmissing\t2\t%s\n' "$phase" "$distribution" "$cache" "$(date --iso-8601=seconds)" >> "$STATUS"
    return 0
  fi

  if [[ "$RESTART_STORAGE_PER_CASE" == "1" ]]; then
    if ! restart_project "$project"; then
      echo "Storage restart failed for $distribution/$cache" | tee "$log_file"
      printf '%s\t%s\t%s\trestart-failed\t3\t%s\n' \
        "$phase" "$distribution" "$cache" "$(date --iso-8601=seconds)" >> "$STATUS"
      return 0
    fi
  fi

  pkill -INT -f 'polygraph_http_server' 2>/dev/null || true
  sleep 1

  if [[ "$phase" == "analysis" ]]; then
    export POLYGRAPH_ALGORITHM_ANALYZER_OUTPUT="$case_out/algorithm.jsonl"
    rm -f "$POLYGRAPH_ALGORITHM_ANALYZER_OUTPUT"
  else
    unset POLYGRAPH_ALGORITHM_ANALYZER_OUTPUT || true
  fi

  log "$phase / $distribution / $cache"
  "$PYTHON_BIN" tools/measurement/benchmark_compact_repeated.py \
    --domain "$domain" \
    --cache-plans "$plan" \
    --request-plan "$request_plan" \
    --repetitions "$repetitions" \
    --output-dir "$case_out" \
    --http-port "$HTTP_PORT" \
    --http-startup-timeout 300 \
    --http-request-timeout "$HTTP_TIMEOUT" \
    --http-server-binary "$server" \
    --project "$project" \
    --overwrite \
    2>&1 | tee "$log_file"
  rc=${PIPESTATUS[0]}

  if [[ $rc -eq 0 ]]; then status=SUCCESS; else status=FAILED; fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$phase" "$distribution" "$cache" "$status" "$rc" "$(date --iso-8601=seconds)" >> "$STATUS"

  if [[ "$phase" == "analysis" && -s "$case_out/algorithm.jsonl" ]]; then
    "$PYTHON_BIN" tools/measurement/summarize_algorithm_analysis.py \
      "$case_out/algorithm.jsonl" \
      --output-prefix "$case_out/algorithm" || true
  fi

  pkill -INT -f 'polygraph_http_server' 2>/dev/null || true
  sleep 1
  return 0
}

[[ -x "$NORMAL_SERVER" || "$RUN_TIMING" != "1" ]] || { echo "Missing $NORMAL_SERVER" >&2; exit 2; }
[[ -x "$ANALYZER_SERVER" || "$RUN_ANALYSIS" != "1" ]] || { echo "Missing $ANALYZER_SERVER" >&2; exit 2; }

log "SF1 DISTRIBUTION MATRIX"
echo "output:                $OUT"
echo "timing repetitions:    $TIMING_REPETITIONS"
echo "analysis repetitions:  $ANALYSIS_REPETITIONS"
echo "logical roots/case:    $ROOT_COUNT"
echo "max depth:             $MAX_DEPTH"
echo "direction:             $DIRECTION"
echo "templates/root:        1 (d${MAX_DEPTH}-${DIRECTION})"
echo "cache modes:           ${CACHE_NAMES[*]}"
echo "BFS execution:         classic"
echo "neighbor batch size:   256"
echo "Arango concurrency:    16"
echo "Arango strategy:       chunked"
echo "Arango scheduler:      ${ARANGO_SERVER_MINIMAL_THREADS}-${ARANGO_SERVER_MAXIMAL_THREADS} threads"

if ! ensure_redis; then
  echo "Redis failed to start" >&2
  exit 3
fi

# SF1 and SF10 use the same published database ports. Keep every volume, but
# guarantee that only the distribution under test owns those ports.
stop_sf10_schema_project
stop_all_matrix_projects

for distribution in "${DISTRIBUTIONS[@]}"; do
  stop_all_matrix_projects
  if ! generate_request_plan "$distribution" >/dev/null; then
    echo "Could not generate request plan for $distribution" >&2
    for cache in "${CACHE_NAMES[@]}"; do
      [[ "$RUN_ANALYSIS" == "1" ]] && printf 'analysis\t%s\t%s\trequest-plan-failed\t4\t%s\n' "$distribution" "$cache" "$(date --iso-8601=seconds)" >> "$STATUS"
      [[ "$RUN_TIMING" == "1" ]] && printf 'timing\t%s\t%s\trequest-plan-failed\t4\t%s\n' "$distribution" "$cache" "$(date --iso-8601=seconds)" >> "$STATUS"
    done
    continue
  fi
  if ! activate_distribution "$distribution"; then
    echo "Could not activate $distribution" >&2
    for cache in "${CACHE_NAMES[@]}"; do
      [[ "$RUN_ANALYSIS" == "1" ]] && printf 'analysis\t%s\t%s\tactivation-failed\t5\t%s\n' "$distribution" "$cache" "$(date --iso-8601=seconds)" >> "$STATUS"
      [[ "$RUN_TIMING" == "1" ]] && printf 'timing\t%s\t%s\tactivation-failed\t5\t%s\n' "$distribution" "$cache" "$(date --iso-8601=seconds)" >> "$STATUS"
    done
    continue
  fi

  for cache in "${CACHE_NAMES[@]}"; do
    if [[ "$RUN_ANALYSIS" == "1" ]]; then
      run_case analysis "$distribution" "$cache" "$ANALYSIS_REPETITIONS" "$ANALYZER_SERVER"
    fi
    if [[ "$RUN_TIMING" == "1" ]]; then
      run_case timing "$distribution" "$cache" "$TIMING_REPETITIONS" "$NORMAL_SERVER"
    fi
  done
done

log "AGGREGATING RESULTS"
"$PYTHON_BIN" - "$OUT" <<'PY'
import csv
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])

# Timing comparison rows.
rows = []
fields = []
for path in sorted((root / "timing").glob("*/*/comparison.csv")):
    distribution = path.parent.parent.name
    cache = path.parent.name
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            row = dict(row)
            row["distribution"] = distribution
            row["matrix_cache"] = cache
            rows.append(row)
            for field in row:
                if field not in fields:
                    fields.append(field)
if rows:
    preferred = ["distribution", "matrix_cache"]
    output_fields = preferred + [field for field in fields if field not in preferred]
    with (root / "timing-comparison.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=output_fields, extrasaction="ignore")
        writer.writeheader(); writer.writerows(rows)

# Analyzer summaries, one compact row per distribution/cache.
analysis_rows = []
for path in sorted((root / "analysis").glob("*/*/algorithm.summary.json")):
    data = json.loads(path.read_text(encoding="utf-8"))
    analysis_rows.append({
        "distribution": path.parent.parent.name,
        "cache": path.parent.name,
        **data.get("totals", {}),
    })
if analysis_rows:
    fields = list(dict.fromkeys(key for row in analysis_rows for key in row))
    with (root / "algorithm-comparison.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader(); writer.writerows(analysis_rows)
PY

log "DONE"
column -t -s $'\t' "$STATUS" 2>/dev/null || cat "$STATUS"
echo
echo "Timing summary:    $OUT/timing-comparison.csv"
echo "Analyzer summary:  $OUT/algorithm-comparison.csv"
echo "Per-query analyzer JSONL lives under: $OUT/analysis/<distribution>/<cache>/algorithm.jsonl"
