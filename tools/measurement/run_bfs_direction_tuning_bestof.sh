#!/usr/bin/env bash
set -uo pipefail

# Tune direction-switch and sparse/bitmap thresholds on the complete MMap cache.
# This deliberately benchmarks only 03-mmap-100, the currently exact pull-capable
# configuration, so the tuning budget is spent where the adaptive algorithm runs.
#
# Usage:
#   tools/measurement/run_bfs_direction_tuning_bestof.sh [repetitions]
#
# Overrides:
#   PYTHON_BIN=/home/akos/miniconda3/bin/python3
#   OUT_ROOT=measurements/my-direction-tuning
#   SKIP_ACTIVATE=1
#   BFS_TUNE_OVERWRITE=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

N="${1:-5}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
OUT_ROOT="${OUT_ROOT:-measurements/sf1-bfs-direction-tuning-$(date +%Y%m%d-%H%M%S)}"
SKIP_ACTIVATE="${SKIP_ACTIVATE:-0}"
OVERWRITE="${BFS_TUNE_OVERWRITE:-0}"
BASE_REQUESTS="${BASE_REQUESTS:-config/benchmarks/sf1-bfs-direction-optimized-http-requests.json}"
CACHE_PLAN="${CACHE_PLAN:-data/ldbc-snb/sf1_0/schema-based/caches/03-mmap-100/cache-plan.json}"
DOMAIN="${DOMAIN:-data/ldbc-snb/sf1_0/schema-based/domain.json}"

if [[ -e "$OUT_ROOT" && "$OVERWRITE" != "1" ]]; then
  echo "error: output already exists: $OUT_ROOT" >&2
  exit 2
fi
if [[ "$OVERWRITE" == "1" ]]; then rm -rf "$OUT_ROOT"; fi
mkdir -p "$OUT_ROOT/_generated" "$OUT_ROOT/logs"

if [[ "$SKIP_ACTIVATE" != "1" ]]; then
  "$PYTHON_BIN" tools/polygraph.py instance 1 schema-based activate
fi

# Curated variants: current default, more/less aggressive pull, and bitmap-vs-sparse controls.
"$PYTHON_BIN" - "$BASE_REQUESTS" "$OUT_ROOT/_generated" <<'PY'
import json, sys
from pathlib import Path
base = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
variants = [
    ("e2-x0-b1k-d5", 2, 0, 1024, 5),
    ("e3-x1-b2k-d5", 3, 1, 2048, 5),
    ("e5-x1-b4k-d10", 5, 1, 4096, 10),
    ("e8-x2-b4k-d10", 8, 2, 4096, 10),
    ("e12-x2-b8k-d10", 12, 2, 8192, 10),
    ("e5-x1-b1k-d2", 5, 1, 1024, 2),
    ("e5-x1-sparse", 5, 1, 1_000_000_000, 100),
]
for name, enter, exit_, bitmap_min, density in variants:
    cfg = json.loads(json.dumps(base))
    payload = cfg.setdefault('defaults', {}).setdefault('payload', {})
    payload['executionPolicy'] = 'direction-optimized'
    payload['pullEnterFrontierPercent'] = enter
    payload['pullExitFrontierPercent'] = exit_
    payload['bitmapFrontierMinSize'] = bitmap_min
    payload['bitmapFrontierDensityPercent'] = density
    path = out / f'{name}.json'
    path.write_text(json.dumps(cfg, indent=2) + '\n', encoding='utf-8')
    print(name)
PY

FAIL=0
for plan in "$OUT_ROOT"/_generated/*.json; do
  name="$(basename "$plan" .json)"
  out="$OUT_ROOT/$name"
  log="$OUT_ROOT/logs/$name.log"
  echo
  echo "=== $name ==="
  set +e
  "$PYTHON_BIN" tools/measurement/benchmark_compact_repeated.py \
    --domain "$DOMAIN" \
    --cache-plans "$CACHE_PLAN" \
    --request-plan "$plan" \
    --repetitions "$N" \
    --output-dir "$out" \
    --http-port 8080 \
    --http-startup-timeout 0 \
    --http-request-timeout 0 \
    --overwrite 2>&1 | tee "$log"
  status=${PIPESTATUS[0]}
  set -e
  if (( status != 0 )); then
    FAIL=1
    echo "[bfs-tune] FAILED: $name" >&2
  fi
done

"$PYTHON_BIN" tools/measurement/analyze_bfs_direction_tuning_bestof.py "$OUT_ROOT" || true
[[ "$FAIL" == "0" ]]
