/**
 * @file include/gpu/GpuExecutionConfig.hpp
 * @brief Runtime-selectable GPU execution/transfer strategies with tuned production defaults.
 */
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace polygraph {

enum class GpuTransferMode {
    /** Preserve the original pageable-host-memory implementation. */
    Baseline,
    /** Use CUDA page-locked host buffers for H2D/D2H transfers. */
    Pinned,
    /** Double-buffer D2H transfers and overlap them with CPU result conversion. */
    Pipelined,
};

[[nodiscard]] constexpr std::string_view to_string(const GpuTransferMode mode) noexcept {
    switch (mode) {
        case GpuTransferMode::Baseline:
            return "baseline";
        case GpuTransferMode::Pinned:
            return "pinned";
        case GpuTransferMode::Pipelined:
            return "pipelined";
    }
    return "baseline";
}

[[nodiscard]] inline GpuTransferMode parse_gpu_transfer_mode(const std::string_view value) {
    if (value == "baseline" || value == "pageable") {
        return GpuTransferMode::Baseline;
    }
    if (value == "pinned") {
        return GpuTransferMode::Pinned;
    }
    if (value == "pipelined" || value == "pipeline") {
        return GpuTransferMode::Pipelined;
    }
    throw std::invalid_argument("GPU transfer mode must be baseline, pinned, or pipelined");
}

/**
 * GPU execution knobs which deliberately change only transport/result handling,
 * not graph semantics. The defaults are the best-performing single-GPU profile
 * measured for PolyGraph (V3 compact, 1 Mi elements per D2H chunk). Individual
 * backends and benchmark runs can still override every field.
 */
struct GpuExecutionConfig {
    GpuTransferMode transfer_mode{GpuTransferMode::Pipelined};

    /** Number of scalar result elements copied in one pipelined D2H chunk. */
    std::size_t d2h_chunk_elements{1U << 20U};

    /**
     * Pin the large host COO arrays while the reusable cuGraph projection is
     * built.  Useful for measuring startup H2D throughput; irrelevant after the
     * persistent projection is resident on the device.
     */
    bool pin_projection_build_inputs{true};

    /**
     * Enable local candidate deduplication before candidates reach the global
     * BFS visited map. Disable this to benchmark the cost of simply streaming
     * duplicate candidates to the normal BFS discovery stage.
     */
    bool candidate_deduplication{true};

    /**
     * For candidate-only one-hop expansion, sort/unique destinations on the GPU
     * before D2H. This can drastically reduce PCIe traffic when many sampled
     * edges converge on the same destination.
     */
    bool device_candidate_compaction{true};

    /**
     * Deduplicate dense destination IDs with a GPU-resident atomic bitmap and
     * compact only first-seen IDs into a device output buffer before D2H.
     * This avoids the O(n log n) device sort while retaining reduced PCIe
     * traffic. Mutually exclusive with device_candidate_compaction.
     */
    bool device_candidate_bitmap_compaction{false};

    /**
     * When neither device compaction strategy is active, use an O(E) dense-ID
     * bitmap on the CPU rather than std::sort/std::unique after the D2H copy.
     */
    bool dense_candidate_deduplication{true};

    /**
     * Keep a per-query GPU visited bitmap for the distributed/frontier BFS path.
     * After the normal device sort+unique, candidates already emitted by this
     * GPU earlier in the same BFS are filtered and marked on-device before D2H.
     * This preserves the CPU/global visited map as the correctness authority,
     * while reducing repeated PCIe traffic across BFS levels.
     */
    bool device_query_visited_filter{false};

    /**
     * Reuse the native cuGraph BFS output buffers across serial queries on the
     * same projection. The workspace is protected by a projection-local mutex,
     * so concurrent native BFS calls serialize rather than corrupt shared state.
     */
    bool native_bfs_reuse_workspace{false};

    /**
     * For native/full-projection cuGraph BFS, compact only reached
     * (dense_vertex, distance) pairs on the GPU and transfer those pairs to the
     * CPU instead of copying the full distance array. Predecessor-producing BFS
     * currently falls back to the normal full-array result path.
     */
    bool native_bfs_compact_result{false};
};

} // namespace polygraph
