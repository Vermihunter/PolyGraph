#!/usr/bin/env bash
set -euo pipefail

# PolyGraph-level sharded multi-GPU benchmark.
# Generates 1/2/4-GPU cache configs from the same base cache policy, then lets
# the normal cache planner materialize GPU:0, GPU:1, ... projections.
#
# Usage:
#   tools/measurement/run_gpu_scale_matrix.sh [repetitions]
#
# Useful overrides:
#   GPU_SCALE_COUNTS=1,2,4
#   GPU_SCALE_MEMORY_MODE=fixed-total   # fair strong scaling (default)
#   GPU_SCALE_MEMORY_MODE=per-gpu       # capacity + compute scaling
#   GPU_SCALE_BASE_CACHE=config/cache/benchmark/sf1/05-gpu20-redis15-mmap15.json
#   GPU_SCALE_BASE_BENCH=config/benchmarks/sf1-gpu-transport.json
#   GPU_SCALE_VARIANT=v3-compact-1m
#   RELOAD_STORAGE=1

N="${1:-10}"
COUNTS="${GPU_SCALE_COUNTS:-1,2,4}"
MEMORY_MODE="${GPU_SCALE_MEMORY_MODE:-fixed-total}"
BASE_CACHE="${GPU_SCALE_BASE_CACHE:-config/cache/benchmark/sf1/05-gpu20-redis15-mmap15.json}"
BASE_BENCH="${GPU_SCALE_BASE_BENCH:-config/benchmarks/sf1-gpu-transport.json}"
BENCHMARK_SH="${POLYGRAPH_BENCHMARK_SH:-tools/measurement/benchmark.sh}"
VARIANT="${GPU_SCALE_VARIANT:-v3-compact-1m}"
BASE_OUT="${GPU_SCALE_OUT:-measurements/sf1-gpu-scale}"
INITIAL_RELOAD="${RELOAD_STORAGE:-0}"

case "$VARIANT" in
    v1-pinned)
        MODE=pinned; CHUNK=1048576; PIN=true; DEDUP=true; COMPACT=false; BITMAP_COMPACT=false; DENSE=true ;;
    v2-pipeline-1m)
        MODE=pipelined; CHUNK=1048576; PIN=true; DEDUP=true; COMPACT=false; BITMAP_COMPACT=false; DENSE=true ;;
    v3-compact-1m)
        MODE=pipelined; CHUNK=1048576; PIN=true; DEDUP=true; COMPACT=true; BITMAP_COMPACT=false; DENSE=true ;;
    v4-bitmap-1m)
        MODE=pipelined; CHUNK=1048576; PIN=true; DEDUP=true; COMPACT=false; BITMAP_COMPACT=true; DENSE=true ;;
    v5-no-dedup-1m)
        MODE=pipelined; CHUNK=1048576; PIN=true; DEDUP=false; COMPACT=false; BITMAP_COMPACT=false; DENSE=false ;;
    *)
        echo "error: GPU_SCALE_VARIANT must be v1-pinned, v2-pipeline-1m, v3-compact-1m, v4-bitmap-1m, or v5-no-dedup-1m" >&2
        exit 2 ;;
esac

python3 tools/measurement/prepare_multigpu_benchmarks.py \
    --base-cache "$BASE_CACHE" \
    --base-benchmark "$BASE_BENCH" \
    --counts "$COUNTS" \
    --memory-mode "$MEMORY_MODE" \
    --measurements-dir "$BASE_OUT" >/tmp/polygraph-gpu-scale-configs.tsv

reload="$INITIAL_RELOAD"
while IFS=$'\t' read -r count cache_cfg bench_cfg; do
    visible=""
    for ((i=0; i<count; ++i)); do
        if [[ -n "$visible" ]]; then visible+=","; fi
        visible+="$i"
    done

    out="$BASE_OUT/g${count}-${MEMORY_MODE}-${VARIANT}"
    mkdir -p "$out"
    cat > "$out/variant.env" <<META
gpus=$count
memory_mode=$MEMORY_MODE
variant=$VARIANT
cache_config=$cache_cfg
benchmark_config=$bench_cfg
cuda_visible_devices=$visible
repetitions=$N
META

    echo
    echo "============================================================"
    echo "POLYGRAPH GPU SCALE: ${count} GPU(s), $MEMORY_MODE, $VARIANT"
    echo "============================================================"

    env \
        CUDA_VISIBLE_DEVICES="$visible" \
        CFG="$bench_cfg" \
        OUT="$out" \
        RELOAD_STORAGE="$reload" \
        POLYGRAPH_GPU_TRANSFER_MODE="$MODE" \
        POLYGRAPH_GPU_D2H_CHUNK_ELEMENTS="$CHUNK" \
        POLYGRAPH_GPU_PIN_PROJECTION_BUILD_INPUTS="$PIN" \
        POLYGRAPH_GPU_CANDIDATE_DEDUPLICATION="$DEDUP" \
        POLYGRAPH_GPU_DEVICE_CANDIDATE_COMPACTION="$COMPACT" \
        POLYGRAPH_GPU_DEVICE_CANDIDATE_BITMAP_COMPACTION="$BITMAP_COMPACT" \
        POLYGRAPH_GPU_DENSE_CANDIDATE_DEDUPLICATION="$DENSE" \
        "$BENCHMARK_SH" "$N" 2>&1 | tee "$out/runner.log"

    reload=0
done < /tmp/polygraph-gpu-scale-configs.tsv

rm -f /tmp/polygraph-gpu-scale-configs.tsv

echo
echo "PolyGraph multi-GPU scale matrix finished: $BASE_OUT"
