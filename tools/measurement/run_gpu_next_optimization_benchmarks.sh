#!/usr/bin/env bash
set -euo pipefail

# Compare the current best distributed GPU frontier path against two next-stage
# optimizations:
#   V6: V3 sort+unique + persistent per-query GPU visited filtering
#   V7+: native/full-projection cuGraph BFS with GPU-resident traversal state,
#        reusable output workspace, and optional reached-result compaction.
#
# Usage:
#   tools/measurement/run_gpu_next_optimization_benchmarks.sh [repetitions]
#
# Optional:
#   GPU_NEXT_OUT=measurements/sf1-gpu-next
#   RELOAD_STORAGE=1
#   GPU_NEXT_RUN_MIXED=1   # additionally compare V3 vs V6 on 01/04/05 GPU plans

N="${1:-30}"
BENCHMARK_SH="${POLYGRAPH_BENCHMARK_SH:-tools/measurement/benchmark.sh}"
BASE_OUT="${GPU_NEXT_OUT:-measurements/sf1-gpu-next}"
FRONTIER_CFG="${GPU_NEXT_FRONTIER_CFG:-config/benchmarks/sf1-gpu-next-frontier.json}"
NATIVE_CFG="${GPU_NEXT_NATIVE_CFG:-config/benchmarks/sf1-gpu-next-native.json}"
MIXED_CFG="${GPU_NEXT_MIXED_CFG:-config/benchmarks/sf1-gpu-next-mixed.json}"
reload="${RELOAD_STORAGE:-0}"

mkdir -p "$BASE_OUT"

run_variant() {
    local name="$1"
    local cfg="$2"
    local query_visited="$3"
    local reuse_workspace="$4"
    local compact_native_result="$5"
    local out="$BASE_OUT/$name"
    local log
    log="$(mktemp "${TMPDIR:-/tmp}/polygraph-gpu-next-${name}.XXXXXX.log")"

    echo
    echo "============================================================"
    echo "GPU NEXT OPTIMIZATION: $name"
    echo "============================================================"
    echo "config:                       $cfg"
    echo "query visited filter:         $query_visited"
    echo "native BFS reuse workspace:   $reuse_workspace"
    echo "native BFS compact result:    $compact_native_result"
    echo "output:                       $out"

    if ! env \
        CFG="$cfg" \
        OUT="$out" \
        RELOAD_STORAGE="$reload" \
        POLYGRAPH_GPU_TRANSFER_MODE=pipelined \
        POLYGRAPH_GPU_D2H_CHUNK_ELEMENTS=1048576 \
        POLYGRAPH_GPU_PIN_PROJECTION_BUILD_INPUTS=true \
        POLYGRAPH_GPU_CANDIDATE_DEDUPLICATION=true \
        POLYGRAPH_GPU_DEVICE_CANDIDATE_COMPACTION=true \
        POLYGRAPH_GPU_DEVICE_CANDIDATE_BITMAP_COMPACTION=false \
        POLYGRAPH_GPU_DENSE_CANDIDATE_DEDUPLICATION=true \
        POLYGRAPH_GPU_DEVICE_QUERY_VISITED_FILTER="$query_visited" \
        POLYGRAPH_GPU_NATIVE_BFS_REUSE_WORKSPACE="$reuse_workspace" \
        POLYGRAPH_GPU_NATIVE_BFS_COMPACT_RESULT="$compact_native_result" \
        "$BENCHMARK_SH" "$N" 2>&1 | tee "$log"; then
        mkdir -p "$out"
        mv "$log" "$out/runner.log"
        echo "error: benchmark failed for $name" >&2
        return 1
    fi

    reload=0
    mkdir -p "$out"
    mv "$log" "$out/runner.log"
    cat > "$out/variant.env" <<META
variant=$name
benchmark_config=$cfg
repetitions=$N
transfer_mode=pipelined
d2h_chunk_elements=1048576
device_candidate_compaction=true
device_query_visited_filter=$query_visited
native_bfs_reuse_workspace=$reuse_workspace
native_bfs_compact_result=$compact_native_result
META
    nvidia-smi -L > "$out/nvidia-smi-L.txt" 2>&1 || true
}

# Same PolyGraph per-level execution, differing only by whether already emitted
# GPU vertices are removed on-device before D2H.
run_variant v3-frontier-compact       "$FRONTIER_CFG" false false false
run_variant v6-frontier-visited       "$FRONTIER_CFG" true  false false

# 01-gpu-100 only. allowNativePushdown=true in the request plan means the whole
# depth-limited BFS is executed by one cugraph::bfs call with no per-level D2H.
run_variant v7-native                 "$NATIVE_CFG" false false false
run_variant v8-native-reuse           "$NATIVE_CFG" false true  false
run_variant v9-native-reuse-compact   "$NATIVE_CFG" false true  true

echo
echo "============================================================"
echo "GPU NEXT OPTIMIZATION ANALYSIS"
echo "============================================================"
python3 tools/measurement/analyze_gpu_transport_matrix.py \
    --root "$BASE_OUT" \
    --config "$FRONTIER_CFG" \
    --baseline v3-frontier-compact

if [[ "${GPU_NEXT_RUN_MIXED:-0}" == "1" ]]; then
    MIXED_OUT="${GPU_NEXT_MIXED_OUT:-measurements/sf1-gpu-next-mixed}"
    echo
    echo "============================================================"
    echo "OPTIONAL HYBRID GPU CACHE TEST: 01 / 04 / 05"
    echo "============================================================"

    for pair in "v3-frontier-compact:false" "v6-frontier-visited:true"; do
        name="${pair%%:*}"
        query_visited="${pair##*:}"
        out="$MIXED_OUT/$name"
        env \
            CFG="$MIXED_CFG" \
            OUT="$out" \
            RELOAD_STORAGE=0 \
            POLYGRAPH_GPU_TRANSFER_MODE=pipelined \
            POLYGRAPH_GPU_D2H_CHUNK_ELEMENTS=1048576 \
            POLYGRAPH_GPU_PIN_PROJECTION_BUILD_INPUTS=true \
            POLYGRAPH_GPU_CANDIDATE_DEDUPLICATION=true \
            POLYGRAPH_GPU_DEVICE_CANDIDATE_COMPACTION=true \
            POLYGRAPH_GPU_DEVICE_CANDIDATE_BITMAP_COMPACTION=false \
            POLYGRAPH_GPU_DENSE_CANDIDATE_DEDUPLICATION=true \
            POLYGRAPH_GPU_DEVICE_QUERY_VISITED_FILTER="$query_visited" \
            POLYGRAPH_GPU_NATIVE_BFS_REUSE_WORKSPACE=false \
            POLYGRAPH_GPU_NATIVE_BFS_COMPACT_RESULT=false \
            "$BENCHMARK_SH" "$N"
    done

    python3 tools/measurement/analyze_gpu_transport_matrix.py \
        --root "$MIXED_OUT" \
        --config "$MIXED_CFG" \
        --baseline v3-frontier-compact

    echo "Mixed-cache comparison CSVs:"
    echo "  $MIXED_OUT/v3-frontier-compact/comparison.csv"
    echo "  $MIXED_OUT/v6-frontier-visited/comparison.csv"
    echo "  $MIXED_OUT/_analysis/summary.txt"
fi
