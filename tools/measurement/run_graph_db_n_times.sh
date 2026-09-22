#!/usr/bin/env bash
set -euo pipefail

# ---------------- CONFIG ----------------
RUNS="${RUNS:-5}"

BIN="${BIN:-../../cmake-build-debug/src/core/graph_db}"
LGS="${LGS:-../../storage_layer/sample-university-domain.graph-shape.json}"
DATA="${DATA:-../../tools/generators/large-university.json}"

# Final output file.
# If RUNS > 1, each run gets its own file: bfs-res_run_01.csv, ...
OUTPUT="${OUTPUT:-graph_run/bfs-res.csv}"

# Optional log file for timings/errors.
LOG="${LOG:-run-log.txt}"

# Extra env vars, useful for profiling.
POLYGRAPH_PROFILE="${POLYGRAPH_PROFILE:-0}"
POLYGRAPH_PROFILE_LAYERS="${POLYGRAPH_PROFILE_LAYERS:-0}"
# ----------------------------------------


mkdir -p "$(dirname "$OUTPUT")" 2>/dev/null || true
: > "$LOG"

base="${OUTPUT%.*}"
ext="${OUTPUT##*.}"

if [[ "$base" == "$OUTPUT" ]]; then
  ext="csv"
fi

echo "Running $RUNS times..." | tee -a "$LOG"
echo "Binary: $BIN" | tee -a "$LOG"
echo "Output base: $OUTPUT" | tee -a "$LOG"
echo | tee -a "$LOG"

for i in $(seq 1 "$RUNS"); do
  run_id=$(printf "%02d" "$i")

  if [[ "$RUNS" -eq 1 ]]; then
    out_file="$OUTPUT"
  else
    out_file="${base}_run_${run_id}.${ext}"
  fi

  echo "=== Run $i / $RUNS ===" | tee -a "$LOG"
  echo "Output: $out_file" | tee -a "$LOG"

  start_ns=$(date +%s%N)

  POLYGRAPH_PROFILE="$POLYGRAPH_PROFILE" \
  POLYGRAPH_PROFILE_LAYERS="$POLYGRAPH_PROFILE_LAYERS" \
  "$BIN" \
    -lgs "$LGS" \
    --data "$DATA" \
    > "$out_file" \
    2>> "$LOG"

  end_ns=$(date +%s%N)
  elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

  echo "Run $i finished in ${elapsed_ms} ms" | tee -a "$LOG"
  echo | tee -a "$LOG"
done

echo "Done." | tee -a "$LOG"