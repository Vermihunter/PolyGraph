#!/usr/bin/env bash
set -euo pipefail

# Native libcugraph multi-GPU strong-scaling harness.
# The graph size and degree remain fixed; only the MPI rank/GPU count changes.
#
# Usage:
#   experiments/distributed_gpu/run_native_mg_matrix.sh
#
# Overrides:
#   MG_COUNTS=1,2,4
#   MG_VERTICES=100000000
#   MG_DEGREE=8
#   MG_ITERATIONS=20
#   MG_BIN=build/experiments/distributed_gpu/polygraph-mg-cugraph-bfs
#   MPI_EXTRA_ARGS='--bind-to none'

BIN="${MG_BIN:-build/experiments/distributed_gpu/polygraph-mg-cugraph-bfs}"
COUNTS="${MG_COUNTS:-1,2,4}"
VERTICES="${MG_VERTICES:-100000000}"
DEGREE="${MG_DEGREE:-8}"
ITERATIONS="${MG_ITERATIONS:-20}"
OUT="${MG_OUT:-measurements/native-mg-cugraph}"
MPI_EXTRA_ARGS="${MPI_EXTRA_ARGS:-}"

[[ -x "$BIN" ]] || { echo "error: missing native MG binary: $BIN" >&2; exit 2; }
mkdir -p "$OUT"

{
    echo "date=$(date --iso-8601=seconds)"
    echo "counts=$COUNTS"
    echo "vertices=$VERTICES"
    echo "degree=$DEGREE"
    echo "iterations=$ITERATIONS"
    nvidia-smi -L || true
    mpirun --version | head -5 || true
} > "$OUT/environment.txt" 2>&1

printf 'gpus,vertices,degree,iterations,line\n' > "$OUT/results.csv"
IFS=',' read -r -a counts <<< "$COUNTS"
for count in "${counts[@]}"; do
    visible=""
    for ((i=0; i<count; ++i)); do
        [[ -n "$visible" ]] && visible+=","
        visible+="$i"
    done

    echo
    echo "native cuGraph MG: $count GPU(s)"
    output=$(CUDA_VISIBLE_DEVICES="$visible" mpirun -np "$count" $MPI_EXTRA_ARGS \
        "$BIN" "$VERTICES" "$DEGREE" "$ITERATIONS")
    echo "$output" | tee "$OUT/g${count}.log"
    last_line=$(echo "$output" | grep 'native-mg-cugraph-bfs' | tail -1)
    printf '%s,%s,%s,%s,"%s"\n' \
        "$count" "$VERTICES" "$DEGREE" "$ITERATIONS" "${last_line//\"/\"\"}" \
        >> "$OUT/results.csv"
done

echo
echo "Native cuGraph MG matrix finished: $OUT/results.csv"
