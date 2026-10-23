#!/usr/bin/env bash
# Build an offline report of a candidate run versus the archived SF1 baseline.
set -euo pipefail
if (( $# < 1 || $# > 2 )); then
  echo "Usage: $0 measurements/NEW_RUN [reports/OUTPUT_DIR]" >&2
  exit 2
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PY="${PYTHON_BIN:-python3}"
BASE="$ROOT/tools/measurement/reporting/baselines/sf1-d7-20261007"
CANDIDATE="$1"
OUTPUT="${2:-reports/compare-$(basename "$CANDIDATE")}"
cd "$ROOT"
exec "$PY" tools/measurement/reporting/polygraph_report.py \
  --baseline "$BASE" \
  --candidate "$CANDIDATE" \
  --baseline-label 'SF1 · Oct 7–8 successful baseline (console-log recovery)' \
  --candidate-label "$(basename "$CANDIDATE")" \
  --output "$OUTPUT"
