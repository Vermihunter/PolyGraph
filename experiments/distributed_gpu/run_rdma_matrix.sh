#!/usr/bin/env bash
set -euo pipefail

# Runs host-buffer vs CUDA-device-buffer MPI ping-pong across a size sweep.
# Build the two binaries first (commands are in README.md).
#
# Single host:
#   MPI_LAUNCH='mpirun -np 2' experiments/distributed_gpu/run_rdma_matrix.sh
#
# Two hosts:
#   MPI_LAUNCH='mpirun -np 2 -H gpu-node-a:1,gpu-node-b:1' \
#     experiments/distributed_gpu/run_rdma_matrix.sh
#
# With UCX diagnostics, pass them in MPI_LAUNCH or export UCX_LOG_LEVEL=info.

HOST_BIN="${HOST_MPI_BIN:-./build/experiments/host-mpi-pingpong}"
CUDA_BIN="${CUDA_MPI_BIN:-./build/experiments/cuda-aware-mpi-pingpong}"
MPI_LAUNCH="${MPI_LAUNCH:-mpirun -np 2}"
ITERATIONS="${RDMA_ITERATIONS:-100}"
SIZES="${RDMA_SIZES:-4096,65536,1048576,16777216,67108864,268435456}"
OUT="${RDMA_OUT:-measurements/rdma-pingpong}"

[[ -x "$HOST_BIN" ]] || { echo "error: missing $HOST_BIN" >&2; exit 2; }
[[ -x "$CUDA_BIN" ]] || { echo "error: missing $CUDA_BIN" >&2; exit 2; }
mkdir -p "$OUT"

{
    echo "date=$(date --iso-8601=seconds)"
    echo "mpi_launch=$MPI_LAUNCH"
    echo "iterations=$ITERATIONS"
    echo "sizes=$SIZES"
    nvidia-smi -L || true
    mpirun --version | head -5 || true
} > "$OUT/environment.txt" 2>&1

printf 'transport,bytes,iterations,output\n' > "$OUT/results.csv"
IFS=',' read -r -a sizes <<< "$SIZES"
for bytes in "${sizes[@]}"; do
    echo "host MPI: $bytes bytes"
    host_output=$(eval "$MPI_LAUNCH \"$HOST_BIN\" \"$bytes\" \"$ITERATIONS\"")
    printf 'host,%s,%s,"%s"\n' "$bytes" "$ITERATIONS" "${host_output//\"/\"\"}" >> "$OUT/results.csv"
    echo "$host_output"

    echo "CUDA-aware MPI: $bytes bytes"
    cuda_output=$(eval "$MPI_LAUNCH \"$CUDA_BIN\" \"$bytes\" \"$ITERATIONS\"")
    printf 'cuda-device,%s,%s,"%s"\n' "$bytes" "$ITERATIONS" "${cuda_output//\"/\"\"}" >> "$OUT/results.csv"
    echo "$cuda_output"
done

echo "RDMA/CUDA-aware MPI matrix finished: $OUT/results.csv"
