#!/usr/bin/env bash
set -euo pipefail

# Benchmarks the same PolyGraph/cache workload while changing only the GPU
# transfer/result strategy through runtime environment overrides.
#
# Usage:
#   tools/measurement/run_gpu_transport_matrix.sh [repetitions]
#
# Useful overrides:
#   CFG=config/benchmarks/sf1-gpu-transport.json
#   GPU_BENCH_OUT=measurements/sf1-gpu-transport
#   GPU_VARIANTS=v0-baseline,v1-pinned,v2-pipeline-1m,v3-compact-1m,v4-bitmap-1m,v5-no-dedup-1m
#   CUDA_VISIBLE_DEVICES=0
#   RELOAD_STORAGE=1
#
# The existing tools/measurement/benchmark.sh is expected to accept CFG, OUT,
# RELOAD_STORAGE and the repetition count as its first positional argument.

N="${1:-10}"
BENCHMARK_SH="${POLYGRAPH_BENCHMARK_SH:-tools/measurement/benchmark.sh}"
CFG="${CFG:-config/benchmarks/sf1-gpu-transport.json}"
BASE_OUT="${GPU_BENCH_OUT:-measurements/sf1-gpu-transport}"
SELECTED="${GPU_VARIANTS:-all}"
INITIAL_RELOAD="${RELOAD_STORAGE:-0}"

if [[ ! -x "$BENCHMARK_SH" ]]; then
    echo "error: benchmark runner is not executable: $BENCHMARK_SH" >&2
    exit 2
fi
if [[ ! -f "$CFG" ]]; then
    echo "error: benchmark config does not exist: $CFG" >&2
    exit 2
fi

# Keep the transport comparison on exactly one physical GPU unless the caller
# intentionally supplied a different mask.
export CUDA_VISIBLE_DEVICES="${CUDA_VISIBLE_DEVICES:-0}"

mkdir -p "$BASE_OUT"

selected() {
    local needle="$1"
    [[ "$SELECTED" == "all" ]] && return 0
    IFS=',' read -r -a values <<< "$SELECTED"
    local value
    for value in "${values[@]}"; do
        [[ "$value" == "$needle" ]] && return 0
    done
    return 1
}

run_variant() {
    local name="$1"
    local mode="$2"
    local chunk="$3"
    local pin="$4"
    local dedup="$5"
    local compact="$6"
    local bitmap_compact="$7"
    local dense="$8"
    local reload="$9"

    selected "$name" || return 0

    local out="$BASE_OUT/$name"
    local runner_log
    runner_log="$(mktemp "${TMPDIR:-/tmp}/polygraph-gpu-${name}.XXXXXX.log")"

    echo
    echo "============================================================"
    echo "GPU BENCHMARK: $name"
    echo "============================================================"
    echo "transfer mode:       $mode"
    echo "D2H chunk elements:  $chunk"
    echo "pin projection:      $pin"
    echo "candidate dedup:     $dedup"
    echo "sort compaction:     $compact"
    echo "bitmap compaction:   $bitmap_compact"
    echo "dense CPU dedup:     $dense"
    echo "output:              $out"
    echo

    # benchmark_compact_repeated.py uses --overwrite and recreates OUT. Keep the
    # live log outside OUT and write metadata only after the benchmark finishes,
    # otherwise those files are deleted at benchmark startup.
    if ! env \
        CFG="$CFG" \
        OUT="$out" \
        RELOAD_STORAGE="$reload" \
        POLYGRAPH_GPU_TRANSFER_MODE="$mode" \
        POLYGRAPH_GPU_D2H_CHUNK_ELEMENTS="$chunk" \
        POLYGRAPH_GPU_PIN_PROJECTION_BUILD_INPUTS="$pin" \
        POLYGRAPH_GPU_CANDIDATE_DEDUPLICATION="$dedup" \
        POLYGRAPH_GPU_DEVICE_CANDIDATE_COMPACTION="$compact" \
        POLYGRAPH_GPU_DEVICE_CANDIDATE_BITMAP_COMPACTION="$bitmap_compact" \
        POLYGRAPH_GPU_DENSE_CANDIDATE_DEDUPLICATION="$dense" \
        "$BENCHMARK_SH" "$N" 2>&1 | tee "$runner_log"; then
        mkdir -p "$out"
        mv "$runner_log" "$out/runner.log"
        echo "error: GPU benchmark failed for $name" >&2
        return 1
    fi

    mkdir -p "$out"
    mv "$runner_log" "$out/runner.log"
    cat > "$out/variant.env" <<META
variant=$name
transfer_mode=$mode
d2h_chunk_elements=$chunk
pin_projection_build_inputs=$pin
candidate_deduplication=$dedup
device_candidate_compaction=$compact
device_candidate_bitmap_compaction=$bitmap_compact
dense_candidate_deduplication=$dense
cuda_visible_devices=$CUDA_VISIBLE_DEVICES
benchmark_config=$CFG
repetitions=$N
META

    nvidia-smi -L > "$out/nvidia-smi-L.txt" 2>&1 || true
    nvidia-smi --query-gpu=index,name,uuid,driver_version,pci.bus_id,memory.total --format=csv \
        > "$out/gpu-info.csv" 2>&1 || true
}

# Reload persistent storage only for the first selected run. The server/GPU
# projection is still rebuilt by the normal benchmark runner for every variant.
reload="$INITIAL_RELOAD"
run_and_clear_reload() {
    run_variant "$@" "$reload"
    reload=0
}

# V3 is the existing GPU sort+unique implementation. V4 replaces the sort with
# an atomic dense-ID bitmap plus compaction. V5 performs no local dedup at all
# and deliberately relies on BfsExecutor's global visited map.
run_and_clear_reload v0-baseline        baseline  1048576 false true  false false false
run_and_clear_reload v1-pinned          pinned    1048576 true  true  false false true
run_and_clear_reload v2-pipeline-64k    pipelined 65536   true  true  false false true
run_and_clear_reload v2-pipeline-256k   pipelined 262144  true  true  false false true
run_and_clear_reload v2-pipeline-1m     pipelined 1048576 true  true  false false true
run_and_clear_reload v2-pipeline-4m     pipelined 4194304 true  true  false false true
run_and_clear_reload v3-compact-64k     pipelined 65536   true  true  true  false true
run_and_clear_reload v3-compact-256k    pipelined 262144  true  true  true  false true
run_and_clear_reload v3-compact-1m      pipelined 1048576 true  true  true  false true
run_and_clear_reload v3-compact-4m      pipelined 4194304 true  true  true  false true
run_and_clear_reload v4-bitmap-64k      pipelined 65536   true  true  false true  true
run_and_clear_reload v4-bitmap-256k     pipelined 262144  true  true  false true  true
run_and_clear_reload v4-bitmap-1m       pipelined 1048576 true  true  false true  true
run_and_clear_reload v4-bitmap-4m       pipelined 4194304 true  true  false true  true
run_and_clear_reload v5-no-dedup-64k    pipelined 65536   true  false false false false
run_and_clear_reload v5-no-dedup-256k   pipelined 262144  true  false false false false
run_and_clear_reload v5-no-dedup-1m     pipelined 1048576 true  false false false false
run_and_clear_reload v5-no-dedup-4m     pipelined 4194304 true  false false false false

echo
echo "GPU transport matrix finished: $BASE_OUT"
echo
echo "============================================================"
echo "GPU BENCHMARK ANALYSIS"
echo "============================================================"
python3 tools/measurement/analyze_gpu_transport_matrix.py \
    --root "$BASE_OUT" \
    --config "$CFG"
