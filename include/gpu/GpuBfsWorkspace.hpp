/**
 * @file include/gpu/GpuBfsWorkspace.hpp
 * @brief Reusable GPU workspaces for frontier filtering and native BFS results.
 */
#pragma once

#include "gpu/GpuTransfer.hpp"

#include <cstddef>
#include <cstdint>
#include <cuda_runtime.h>
#include <rmm/cuda_stream_view.hpp>
#include <rmm/device_uvector.hpp>

namespace polygraph {

template <typename vertex_t>
struct GpuQueryVisitedWorkspace {
    explicit GpuQueryVisitedWorkspace(const std::size_t vertex_count,
                                      const rmm::cuda_stream_view stream) :
        visited_words((vertex_count + 31U) / 32U, stream),
        filtered_candidates(vertex_count, stream),
        filtered_count(1, stream) {
        reset(stream);
    }

    void reset(const rmm::cuda_stream_view stream) {
        if (visited_words.size() != 0) {
            cuda_check(cudaMemsetAsync(visited_words.data(), 0,
                                       visited_words.size() * sizeof(std::uint32_t), stream.value()),
                       "reset GPU query visited bitmap");
        }
        cuda_check(cudaMemsetAsync(filtered_count.data(), 0, sizeof(std::uint64_t), stream.value()),
                   "reset GPU query filtered counter");
    }

    rmm::device_uvector<std::uint32_t> visited_words;
    rmm::device_uvector<vertex_t> filtered_candidates;
    rmm::device_uvector<std::uint64_t> filtered_count;
};

template <typename vertex_t>
struct NativeGpuBfsWorkspace {
    explicit NativeGpuBfsWorkspace(const std::size_t vertex_count,
                                   const rmm::cuda_stream_view stream) :
        distances(vertex_count, stream),
        predecessors(vertex_count, stream),
        sources(1, stream),
        compact_vertices(vertex_count, stream),
        compact_distances(vertex_count, stream),
        compact_count(1, stream) {}

    rmm::device_uvector<vertex_t> distances;
    rmm::device_uvector<vertex_t> predecessors;
    rmm::device_uvector<vertex_t> sources;
    rmm::device_uvector<vertex_t> compact_vertices;
    rmm::device_uvector<vertex_t> compact_distances;
    rmm::device_uvector<std::uint64_t> compact_count;
};

} // namespace polygraph
