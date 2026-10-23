#!/usr/bin/env bash
set -euo pipefail

# Run every SF1 cache configuration under both BFS policies, one configuration
# per child process so a failure never aborts the rest of the matrix.
#
# Usage:
#   tools/measurement/run_bfs_policy_matrix_bestof.sh [repetitions]
#
# Useful overrides:
#   PYTHON_BIN=/home/akos/miniconda3/bin/python3
#   OUT_ROOT=measurements/my-bfs-policy-run
#   BASE_CFG=config/benchmarks/sf1-cache-matrix.json
#   SKIP_ACTIVATE=1
#   SKIP_REDIS_START=1
#   CACHE_NAMES=01-gpu-100,03-mmap-100,08-no-cache
#   BFS_POLICY_COLD_RESTART_ARANGO=1
#   BFS_POLICY_OVERWRITE=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

N="${1:-10}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
BASE_CFG="${BASE_CFG:-config/benchmarks/sf1-cache-matrix.json}"
CLASSIC_REQUESTS="${CLASSIC_REQUESTS:-config/benchmarks/sf1-bfs-classic-http-requests.json}"
OPT_REQUESTS="${OPT_REQUESTS:-config/benchmarks/sf1-bfs-direction-optimized-http-requests.json}"
OUT_ROOT="${OUT_ROOT:-measurements/sf1-bfs-policy-bestof-$(date +%Y%m%d-%H%M%S)}"
SKIP_ACTIVATE="${SKIP_ACTIVATE:-0}"
SKIP_REDIS_START="${SKIP_REDIS_START:-0}"
CACHE_NAMES="${CACHE_NAMES:-}"
COLD_RESTART_ARANGO="${BFS_POLICY_COLD_RESTART_ARANGO:-0}"
OVERWRITE="${BFS_POLICY_OVERWRITE:-0}"

if ! [[ "$N" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: repetitions must be a positive integer" >&2
  exit 2
fi

for f in "$BASE_CFG" "$CLASSIC_REQUESTS" "$OPT_REQUESTS"; do
  if [[ ! -f "$f" ]]; then
    echo "error: missing $f" >&2
    exit 2
  fi
done

if [[ -e "$OUT_ROOT" && "$OVERWRITE" != "1" ]]; then
  echo "error: output already exists: $OUT_ROOT" >&2
  echo "       choose OUT_ROOT=... or set BFS_POLICY_OVERWRITE=1" >&2
  exit 2
fi
if [[ "$OVERWRITE" == "1" ]]; then
  rm -rf "$OUT_ROOT"
fi
mkdir -p "$OUT_ROOT/logs" "$OUT_ROOT/_generated"

# Read the matrix once.  Emit shell-safe lines rather than duplicating the
# cache/path-selection rules from run_cache_matrix.py in bash.
mapfile -t MATRIX < <("$PYTHON_BIN" - "$BASE_CFG" "$CACHE_NAMES" <<'PY'
import glob, json, shlex, sys
from pathlib import Path

cfg_path, requested = sys.argv[1:]
root = Path.cwd()
cfg = json.loads(Path(cfg_path).read_text(encoding='utf-8'))

def repo_path(value: str) -> Path:
    p = Path(value).expanduser()
    return p.resolve() if p.is_absolute() else (root / p).resolve()

pattern = str(cfg['cacheConfigGlob'])
pattern = pattern if Path(pattern).is_absolute() else str(root / pattern)
configs = [Path(p).resolve() for p in sorted(glob.glob(pattern))]
configured = cfg.get('cacheConfigNames')
if configured is not None:
    by_name = {p.stem: p for p in configs}
    configs = [by_name[name] for name in configured]
if requested.strip():
    wanted = [x.strip() for x in requested.split(',') if x.strip()]
    by_name = {p.stem: p for p in configs}
    missing = [x for x in wanted if x not in by_name]
    if missing:
        raise SystemExit('Unknown CACHE_NAMES: ' + ', '.join(missing))
    configs = [by_name[x] for x in wanted]
if not configs:
    raise SystemExit('No cache configs selected')

values = {
    'SF': str(cfg.get('scaleFactor', '1')),
    'DIST': str(cfg.get('distribution', 'schema-based')),
    'DOMAIN': str(repo_path(str(cfg['domain']))),
    'CACHE_ROOT': str(repo_path(str(cfg['cacheRoot']))),
    'HTTP_PORT': str(int(cfg.get('httpPort', 8080))),
    'HTTP_STARTUP_TIMEOUT': str(float(cfg.get('httpStartupTimeoutSeconds', 0.0))),
    'HTTP_REQUEST_TIMEOUT': str(float(cfg.get('httpRequestTimeoutSeconds', 0.0))),
    'REDIS_ENV': str(repo_path(str(cfg.get('redisEnvFile', 'storage_layer/redis/.env.sf1-benchmark')))),
}
for key, value in values.items():
    print(f'{key}={shlex.quote(value)}')
for config in configs:
    plan = repo_path(str(cfg['cacheRoot'])) / config.stem / 'cache-plan.json'
    if not plan.is_file():
        raise SystemExit(f'Missing cache plan: {plan}')
    print('CACHE=' + shlex.quote(config.stem + '|' + str(plan)))
PY
)

SF="" DIST="" DOMAIN="" CACHE_ROOT="" HTTP_PORT="8080" HTTP_STARTUP_TIMEOUT="0" HTTP_REQUEST_TIMEOUT="0" REDIS_ENV=""
CACHES=()
for line in "${MATRIX[@]}"; do
  case "$line" in
    SF=*) eval "$line" ;;
    DIST=*) eval "$line" ;;
    DOMAIN=*) eval "$line" ;;
    CACHE_ROOT=*) eval "$line" ;;
    HTTP_PORT=*) eval "$line" ;;
    HTTP_STARTUP_TIMEOUT=*) eval "$line" ;;
    HTTP_REQUEST_TIMEOUT=*) eval "$line" ;;
    REDIS_ENV=*) eval "$line" ;;
    CACHE=*)
      eval "$line"
      CACHES+=("$CACHE")
      ;;
  esac
done

if [[ "$SKIP_ACTIVATE" != "1" ]]; then
  echo "+ $PYTHON_BIN tools/polygraph.py instance $SF $DIST activate"
  "$PYTHON_BIN" tools/polygraph.py instance "$SF" "$DIST" activate
fi

if [[ "$SKIP_REDIS_START" != "1" ]]; then
  echo "+ docker compose --env-file $REDIS_ENV -f storage_layer/redis/docker-compose.yml up -d --wait"
  docker compose --env-file "$REDIS_ENV" -f storage_layer/redis/docker-compose.yml up -d --wait
fi

cat > "$OUT_ROOT/run.env" <<ENV
repetitions=$N
base_config=$BASE_CFG
classic_requests=$CLASSIC_REQUESTS
optimized_requests=$OPT_REQUESTS
skip_activate=$SKIP_ACTIVATE
skip_redis_start=$SKIP_REDIS_START
cold_restart_arango=$COLD_RESTART_ARANGO
http_port=$HTTP_PORT
http_request_timeout=$HTTP_REQUEST_TIMEOUT
ENV

printf '[bfs-policy] selected cache configs (%d):\n' "${#CACHES[@]}"
for entry in "${CACHES[@]}"; do
  echo "  - ${entry%%|*}"
done

declare -a FAILURES=()
run_one() {
  local policy="$1"
  local request_plan="$2"
  local cache_name="$3"
  local cache_plan="$4"
  local out="$OUT_ROOT/$policy/$cache_name"
  local log="$OUT_ROOT/logs/${policy}__${cache_name}.log"

  mkdir -p "$(dirname "$out")"
  local cmd=(
    "$PYTHON_BIN" tools/measurement/benchmark_compact_repeated.py
    --domain "$DOMAIN"
    --cache-plans "$cache_plan"
    --request-plan "$request_plan"
    --repetitions "$N"
    --output-dir "$out"
    --http-port "$HTTP_PORT"
    --http-startup-timeout "$HTTP_STARTUP_TIMEOUT"
    --http-request-timeout "$HTTP_REQUEST_TIMEOUT"
    --overwrite
  )
  if [[ "$COLD_RESTART_ARANGO" == "1" ]]; then
    cmd+=(--cold-restart-arango-before-each-cache)
  fi

  echo
  echo "============================================================"
  echo "RUNNING $policy / $cache_name"
  echo "============================================================"
  printf '+ '
  printf '%q ' "${cmd[@]}"
  printf '\n'

  set +e
  "${cmd[@]}" 2>&1 | tee "$log"
  local status=${PIPESTATUS[0]}
  set -e
  if (( status != 0 )); then
    FAILURES+=("$policy/$cache_name")
    echo "[bfs-policy] FAILED: $policy/$cache_name (continuing)" >&2
  fi
}

# Keep the same ordering for both policies to make raw comparisons intuitive.
# For a publication-quality run, repeat with reversed policy/config order too.
set -e
for policy in classic direction-optimized; do
  if [[ "$policy" == "classic" ]]; then
    request_plan="$CLASSIC_REQUESTS"
  else
    request_plan="$OPT_REQUESTS"
  fi
  for entry in "${CACHES[@]}"; do
    cache_name="${entry%%|*}"
    cache_plan="${entry#*|}"
    run_one "$policy" "$request_plan" "$cache_name" "$cache_plan"
  done
done

"$PYTHON_BIN" tools/measurement/analyze_bfs_policy_matrix_bestof.py "$OUT_ROOT" || true

echo
echo "============================================================"
echo "BFS POLICY MATRIX FINISHED"
echo "============================================================"
echo "output:   $OUT_ROOT"
echo "analysis: $OUT_ROOT/_analysis/summary.md"
if (( ${#FAILURES[@]} > 0 )); then
  echo "failed runs (${#FAILURES[@]}):"
  printf '  - %s\n' "${FAILURES[@]}"
  exit 1
fi
