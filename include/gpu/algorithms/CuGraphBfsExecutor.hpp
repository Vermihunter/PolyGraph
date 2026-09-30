/**
 * @file include/gpu/algorithms/CuGraphBfsExecutor.hpp
 * @brief cuGraph BFS with reusable device workspaces and compact result transfer.
 */
#ifndef POLYGRAPH_CUGRAPHBFSEXECUTOR_HPP
#define POLYGRAPH_CUGRAPHBFSEXECUTOR_HPP

#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "events/Events.hpp"
#include "gpu/GpuTransfer.hpp"
#include "gpu/algorithms/DeviceCompaction.hpp"
#include "gpu/data_model/CsrGraphProjection.hpp"

#include <algorithm>
#include <cstdint>
#include <cugraph/algorithms.hpp>
#include <limits>
#include <mutex>
#include <raft/util/cudart_utils.hpp>
#include <rmm/device_uvector.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace polygraph {
class CuGraphBfsExecutor {
  public:
    template <typename vertex_t, typename edge_t, bool MULTI_GPU = false>
    static BfsResult run(CsrGraphProjection<vertex_t, edge_t>& projection,
                         const GlobalId& source_id,
                         const BfsOptions& options) {
        CudaDeviceGuard device_guard(projection.device_id());
        const auto& execution = projection.execution_config();
        EVENT_SCOPE_INFO(total_span, "cugraph.bfs", {"projection_vertices", projection.vertex_count()},
                         {"return_predecessors", options.return_predecessors},
                         {"transfer_mode", std::string(to_string(execution.transfer_mode))},
                         {"reuse_workspace", execution.native_bfs_reuse_workspace},
                         {"compact_result", execution.native_bfs_compact_result});
        if (!projection.mapper().contains(source_id)) {
            throw std::runtime_error("Source vertex is not present in this GPU projection");
        }

        vertex_t source_dense{};
        {
            EVENT_SCOPE_INFO(map_span, "cugraph.bfs.source_to_gpu_id");
            source_dense = projection.mapper().at(source_id);
            EVENT_SCOPE_INFO_FIELD(map_span, "dense_source", source_dense);
        }

        const std::size_t n = projection.vertex_count();
        if (n == 0) {
            throw std::runtime_error("GPU projection has zero vertices");
        }

        BfsResult result;
        if (execution.native_bfs_reuse_workspace) {
            // One reusable device workspace per projection avoids repeated RMM
            // allocations. Serializing only native-BFS calls keeps shared buffers
            // safe without affecting the distributed one-hop path.
            std::unique_lock workspace_lock(projection.native_bfs_workspace_mutex());
            auto& workspace = projection.native_bfs_workspace();
            result = run_with_buffers<vertex_t, edge_t, MULTI_GPU>(
                    projection, source_dense, options,
                    workspace.distances, workspace.predecessors, workspace.sources,
                    &workspace.compact_vertices, &workspace.compact_distances, &workspace.compact_count);
        } else {
            auto stream = projection.handle().get_stream();
            auto d_distances = rmm::device_uvector<vertex_t>(n, stream);
            auto d_predecessors = rmm::device_uvector<vertex_t>(n, stream);
            auto d_sources = rmm::device_uvector<vertex_t>(1, stream);
            result = run_with_buffers<vertex_t, edge_t, MULTI_GPU>(
                    projection, source_dense, options, d_distances, d_predecessors, d_sources,
                    nullptr, nullptr, nullptr);
        }

        EVENT_SCOPE_INFO_FIELD(total_span, "result_vertices", result.distance.size());
        EVENT_SCOPE_INFO_FIELD(total_span, "result_predecessors", result.predecessor.size());
        return result;
    }

  private:
    template <typename vertex_t, typename edge_t, bool MULTI_GPU>
    static BfsResult run_with_buffers(CsrGraphProjection<vertex_t, edge_t>& projection,
                                      const vertex_t source_dense,
                                      const BfsOptions& options,
                                      rmm::device_uvector<vertex_t>& d_distances,
                                      rmm::device_uvector<vertex_t>& d_predecessors,
                                      rmm::device_uvector<vertex_t>& d_sources,
                                      rmm::device_uvector<vertex_t>* reusable_compact_vertices,
                                      rmm::device_uvector<vertex_t>* reusable_compact_distances,
                                      rmm::device_uvector<std::uint64_t>* reusable_compact_count) {
        auto& handle = projection.handle();
        auto stream = handle.get_stream();
        const auto& execution = projection.execution_config();
        const std::size_t n = projection.vertex_count();

        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.bfs.source_h2d_enqueue", {"bytes", sizeof(vertex_t)});
            raft::update_device(d_sources.data(), &source_dense, 1, stream);
        }

        const vertex_t depth_limit = options.max_depth.has_value()
                                         ? static_cast<vertex_t>(*options.max_depth)
                                         : std::numeric_limits<vertex_t>::max();
        vertex_t* predecessors_ptr = options.return_predecessors ? d_predecessors.data() : nullptr;

        {
            EVENT_SCOPE_INFO(kernel_span, "cugraph.bfs.kernel_api", {"depth_limit", depth_limit});
            cugraph::bfs<vertex_t, edge_t, MULTI_GPU>(handle,
                                                      projection.view(),
                                                      d_distances.data(),
                                                      predecessors_ptr,
                                                      d_sources.data(),
                                                      1,
                                                      false,
                                                      depth_limit,
                                                      false);
        }

        if (execution.native_bfs_compact_result && !options.return_predecessors) {
            const vertex_t compact_max_distance = options.max_depth.has_value()
                                                      ? depth_limit
                                                      : static_cast<vertex_t>(n - 1U);
            return compact_and_copy_reached(projection, d_distances, compact_max_distance,
                                            reusable_compact_vertices,
                                            reusable_compact_distances,
                                            reusable_compact_count);
        }

        switch (execution.transfer_mode) {
            case GpuTransferMode::Baseline:
                return copy_pageable(projection, d_distances, d_predecessors, options.return_predecessors);
            case GpuTransferMode::Pinned:
                return copy_pinned(projection, d_distances, d_predecessors, options.return_predecessors);
            case GpuTransferMode::Pipelined:
                return copy_pipelined(projection, d_distances, d_predecessors, options.return_predecessors,
                                      execution.d2h_chunk_elements);
        }
        throw std::runtime_error("Unknown GPU transfer mode");
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult compact_and_copy_reached(CsrGraphProjection<vertex_t, edge_t>& projection,
                                              const rmm::device_uvector<vertex_t>& d_distances,
                                              const vertex_t max_distance,
                                              rmm::device_uvector<vertex_t>* reusable_vertices,
                                              rmm::device_uvector<vertex_t>* reusable_distances,
                                              rmm::device_uvector<std::uint64_t>* reusable_count) {
        auto& handle = projection.handle();
        auto stream = handle.get_stream();
        const std::size_t n = projection.vertex_count();

        if (reusable_vertices && reusable_distances && reusable_count) {
            return compact_and_copy_reached_impl(projection, d_distances, max_distance,
                                                 *reusable_vertices, *reusable_distances, *reusable_count);
        }

        auto d_vertices = rmm::device_uvector<vertex_t>(n, stream);
        auto d_compact_distances = rmm::device_uvector<vertex_t>(n, stream);
        auto d_count = rmm::device_uvector<std::uint64_t>(1, stream);
        return compact_and_copy_reached_impl(projection, d_distances, max_distance,
                                             d_vertices, d_compact_distances, d_count);
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult compact_and_copy_reached_impl(CsrGraphProjection<vertex_t, edge_t>& projection,
                                                   const rmm::device_uvector<vertex_t>& d_distances,
                                                   const vertex_t max_distance,
                                                   rmm::device_uvector<vertex_t>& d_vertices,
                                                   rmm::device_uvector<vertex_t>& d_compact_distances,
                                                   rmm::device_uvector<std::uint64_t>& d_count) {
        auto& handle = projection.handle();
        auto stream = handle.get_stream();
        const auto& execution = projection.execution_config();
        const std::size_t n = projection.vertex_count();

        std::uint64_t reached_u64 = 0;
        {
            EVENT_SCOPE_INFO(compact_span, "cugraph.bfs.compact_reached",
                             {"projection_vertices", n});
            cuda_check(cudaMemsetAsync(d_count.data(), 0, sizeof(std::uint64_t), stream),
                       "reset native BFS compact counter");
            gpu_detail::compact_reached_bfs_device(d_distances.data(), n,
                                                   d_vertices.data(), d_compact_distances.data(),
                                                   d_count.data(), max_distance, stream);
            cuda_check(cudaMemcpyAsync(&reached_u64, d_count.data(), sizeof(reached_u64),
                                       cudaMemcpyDeviceToHost, stream),
                       "native BFS compact count D2H");
            handle.sync_stream();
            EVENT_SCOPE_INFO_FIELD(compact_span, "reached_vertices", reached_u64);
        }

        const auto reached = static_cast<std::size_t>(reached_u64);
        if (reached == 0) {
            return {};
        }

        switch (execution.transfer_mode) {
            case GpuTransferMode::Baseline:
                return copy_compacted_pageable(projection, d_vertices, d_compact_distances, reached);
            case GpuTransferMode::Pinned:
                return copy_compacted_pinned(projection, d_vertices, d_compact_distances, reached);
            case GpuTransferMode::Pipelined:
                return copy_compacted_pipelined(projection, d_vertices, d_compact_distances, reached,
                                                execution.d2h_chunk_elements);
        }
        throw std::runtime_error("Unknown GPU transfer mode");
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult copy_pageable(CsrGraphProjection<vertex_t, edge_t>& projection,
                                   const rmm::device_uvector<vertex_t>& d_distances,
                                   const rmm::device_uvector<vertex_t>& d_predecessors,
                                   const bool has_predecessors) {
        auto& handle = projection.handle();
        auto stream = handle.get_stream();
        const auto n = projection.vertex_count();
        std::vector<vertex_t> h_distances(n);
        std::vector<vertex_t> h_predecessors(has_predecessors ? n : 0U);
        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.bfs.results_d2h_enqueue",
                             {"mode", "baseline"},
                             {"distance_bytes", n * sizeof(vertex_t)},
                             {"predecessor_bytes", has_predecessors ? n * sizeof(vertex_t) : 0U});
            raft::update_host(h_distances.data(), d_distances.data(), n, stream);
            if (has_predecessors) {
                raft::update_host(h_predecessors.data(), d_predecessors.data(), n, stream);
            }
            handle.sync_stream();
        }
        return to_external_result(projection, h_distances.data(), h_predecessors.data(), n, has_predecessors, 0);
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult copy_pinned(CsrGraphProjection<vertex_t, edge_t>& projection,
                                 const rmm::device_uvector<vertex_t>& d_distances,
                                 const rmm::device_uvector<vertex_t>& d_predecessors,
                                 const bool has_predecessors) {
        auto& handle = projection.handle();
        auto stream = handle.get_stream();
        const auto n = projection.vertex_count();
        PinnedHostBuffer<vertex_t> h_distances(n);
        PinnedHostBuffer<vertex_t> h_predecessors(has_predecessors ? n : 0U);
        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.bfs.results_d2h_enqueue",
                             {"mode", "pinned"},
                             {"distance_bytes", n * sizeof(vertex_t)},
                             {"predecessor_bytes", has_predecessors ? n * sizeof(vertex_t) : 0U});
            cuda_check(cudaMemcpyAsync(h_distances.data(), d_distances.data(), n * sizeof(vertex_t),
                                       cudaMemcpyDeviceToHost, stream),
                       "BFS pinned distances D2H");
            if (has_predecessors) {
                cuda_check(cudaMemcpyAsync(h_predecessors.data(), d_predecessors.data(), n * sizeof(vertex_t),
                                           cudaMemcpyDeviceToHost, stream),
                           "BFS pinned predecessors D2H");
            }
            handle.sync_stream();
        }
        return to_external_result(projection, h_distances.data(), h_predecessors.data(), n, has_predecessors, 0);
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult copy_pipelined(CsrGraphProjection<vertex_t, edge_t>& projection,
                                    const rmm::device_uvector<vertex_t>& d_distances,
                                    const rmm::device_uvector<vertex_t>& d_predecessors,
                                    const bool has_predecessors,
                                    const std::size_t requested_chunk) {
        EVENT_SCOPE_INFO(pipe_span, "cugraph.bfs.results_d2h_pipeline",
                         {"vertices", projection.vertex_count()}, {"chunk_elements", requested_chunk});
        const std::size_t n = projection.vertex_count();
        const std::size_t chunk = std::max<std::size_t>(1, std::min(requested_chunk, n));
        auto compute_stream = projection.handle().get_stream();

        CudaEvent kernel_done;
        cuda_check(cudaEventRecord(kernel_done.get(), compute_stream), "record BFS kernel completion");

        CudaStream copy_streams[2];
        CudaEvent ready[2];
        for (auto& copy_stream : copy_streams) {
            cuda_check(cudaStreamWaitEvent(copy_stream.get(), kernel_done.get(), 0), "BFS copy stream wait");
        }
        PinnedHostBuffer<vertex_t> distance_slots[2]{PinnedHostBuffer<vertex_t>(chunk),
                                                     PinnedHostBuffer<vertex_t>(chunk)};
        PinnedHostBuffer<vertex_t> predecessor_slots[2];
        if (has_predecessors) {
            predecessor_slots[0].resize(chunk);
            predecessor_slots[1].resize(chunk);
        }

        struct PendingChunk { std::size_t offset{0}; std::size_t count{0}; bool valid{false}; };
        PendingChunk pending[2];
        BfsResult result;

        auto consume = [&](const int slot) {
            if (!pending[slot].valid) return;
            cuda_check(cudaEventSynchronize(ready[slot].get()), "wait BFS D2H chunk");
            append_external_result(result, projection, distance_slots[slot].data(),
                                   has_predecessors ? predecessor_slots[slot].data() : nullptr,
                                   pending[slot].count, has_predecessors, pending[slot].offset);
            pending[slot].valid = false;
        };

        std::size_t chunk_index = 0;
        for (std::size_t offset = 0; offset < n; offset += chunk, ++chunk_index) {
            const int slot = static_cast<int>(chunk_index & 1U);
            consume(slot);
            const std::size_t count = std::min(chunk, n - offset);
            const auto s = copy_streams[slot].get();
            cuda_check(cudaMemcpyAsync(distance_slots[slot].data(), d_distances.data() + offset,
                                       count * sizeof(vertex_t), cudaMemcpyDeviceToHost, s),
                       "BFS pipelined distance D2H");
            if (has_predecessors) {
                cuda_check(cudaMemcpyAsync(predecessor_slots[slot].data(), d_predecessors.data() + offset,
                                           count * sizeof(vertex_t), cudaMemcpyDeviceToHost, s),
                           "BFS pipelined predecessor D2H");
            }
            cuda_check(cudaEventRecord(ready[slot].get(), s), "record BFS D2H chunk");
            pending[slot] = PendingChunk{offset, count, true};
        }
        consume(0);
        consume(1);
        return result;
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult copy_compacted_pageable(CsrGraphProjection<vertex_t, edge_t>& projection,
                                             const rmm::device_uvector<vertex_t>& d_vertices,
                                             const rmm::device_uvector<vertex_t>& d_distances,
                                             const std::size_t count) {
        auto& handle = projection.handle();
        auto stream = handle.get_stream();
        std::vector<vertex_t> h_vertices(count);
        std::vector<vertex_t> h_distances(count);
        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.bfs.compact_results_d2h",
                             {"mode", "baseline"}, {"reached_vertices", count},
                             {"bytes", count * 2U * sizeof(vertex_t)});
            raft::update_host(h_vertices.data(), d_vertices.data(), count, stream);
            raft::update_host(h_distances.data(), d_distances.data(), count, stream);
            handle.sync_stream();
        }
        return compacted_to_external_result(projection, h_vertices.data(), h_distances.data(), count);
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult copy_compacted_pinned(CsrGraphProjection<vertex_t, edge_t>& projection,
                                           const rmm::device_uvector<vertex_t>& d_vertices,
                                           const rmm::device_uvector<vertex_t>& d_distances,
                                           const std::size_t count) {
        auto& handle = projection.handle();
        auto stream = handle.get_stream();
        PinnedHostBuffer<vertex_t> h_vertices(count);
        PinnedHostBuffer<vertex_t> h_distances(count);
        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.bfs.compact_results_d2h",
                             {"mode", "pinned"}, {"reached_vertices", count},
                             {"bytes", count * 2U * sizeof(vertex_t)});
            cuda_check(cudaMemcpyAsync(h_vertices.data(), d_vertices.data(), count * sizeof(vertex_t),
                                       cudaMemcpyDeviceToHost, stream),
                       "native BFS compact vertices D2H");
            cuda_check(cudaMemcpyAsync(h_distances.data(), d_distances.data(), count * sizeof(vertex_t),
                                       cudaMemcpyDeviceToHost, stream),
                       "native BFS compact distances D2H");
            handle.sync_stream();
        }
        return compacted_to_external_result(projection, h_vertices.data(), h_distances.data(), count);
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult copy_compacted_pipelined(CsrGraphProjection<vertex_t, edge_t>& projection,
                                              const rmm::device_uvector<vertex_t>& d_vertices,
                                              const rmm::device_uvector<vertex_t>& d_distances,
                                              const std::size_t count,
                                              const std::size_t requested_chunk) {
        EVENT_SCOPE_INFO(pipe_span, "cugraph.bfs.compact_results_d2h_pipeline",
                         {"reached_vertices", count}, {"chunk_elements", requested_chunk});
        const std::size_t chunk = std::max<std::size_t>(1, std::min(requested_chunk, count));
        auto compute_stream = projection.handle().get_stream();
        CudaEvent compact_done;
        cuda_check(cudaEventRecord(compact_done.get(), compute_stream), "record BFS compact completion");

        CudaStream copy_streams[2];
        CudaEvent ready[2];
        for (auto& copy_stream : copy_streams) {
            cuda_check(cudaStreamWaitEvent(copy_stream.get(), compact_done.get(), 0),
                       "compact BFS copy stream wait");
        }
        PinnedHostBuffer<vertex_t> vertex_slots[2]{PinnedHostBuffer<vertex_t>(chunk),
                                                   PinnedHostBuffer<vertex_t>(chunk)};
        PinnedHostBuffer<vertex_t> distance_slots[2]{PinnedHostBuffer<vertex_t>(chunk),
                                                     PinnedHostBuffer<vertex_t>(chunk)};
        struct PendingChunk { std::size_t count{0}; bool valid{false}; };
        PendingChunk pending[2];
        BfsResult result;

        auto consume = [&](const int slot) {
            if (!pending[slot].valid) return;
            cuda_check(cudaEventSynchronize(ready[slot].get()), "wait compact BFS D2H chunk");
            append_compacted_external_result(result, projection, vertex_slots[slot].data(),
                                             distance_slots[slot].data(), pending[slot].count);
            pending[slot].valid = false;
        };

        std::size_t chunk_index = 0;
        for (std::size_t offset = 0; offset < count; offset += chunk, ++chunk_index) {
            const int slot = static_cast<int>(chunk_index & 1U);
            consume(slot);
            const std::size_t current = std::min(chunk, count - offset);
            const auto s = copy_streams[slot].get();
            cuda_check(cudaMemcpyAsync(vertex_slots[slot].data(), d_vertices.data() + offset,
                                       current * sizeof(vertex_t), cudaMemcpyDeviceToHost, s),
                       "compact BFS vertices D2H");
            cuda_check(cudaMemcpyAsync(distance_slots[slot].data(), d_distances.data() + offset,
                                       current * sizeof(vertex_t), cudaMemcpyDeviceToHost, s),
                       "compact BFS distances D2H");
            cuda_check(cudaEventRecord(ready[slot].get(), s), "record compact BFS D2H chunk");
            pending[slot] = PendingChunk{current, true};
        }
        consume(0);
        consume(1);
        return result;
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult compacted_to_external_result(const CsrGraphProjection<vertex_t, edge_t>& projection,
                                                  const vertex_t* h_vertices,
                                                  const vertex_t* h_distances,
                                                  const std::size_t count) {
        BfsResult result;
        result.distance.reserve(count);
        append_compacted_external_result(result, projection, h_vertices, h_distances, count);
        return result;
    }

    template <typename vertex_t, typename edge_t>
    static void append_compacted_external_result(BfsResult& result,
                                                 const CsrGraphProjection<vertex_t, edge_t>& projection,
                                                 const vertex_t* h_vertices,
                                                 const vertex_t* h_distances,
                                                 const std::size_t count) {
        EVENT_SCOPE_INFO(conversion_span, "cugraph.bfs_compact_to_intermediate_conversion",
                         {"reached_vertices", count});
        for (std::size_t i = 0; i < count; ++i) {
            const vertex_t dense = h_vertices[i];
            const vertex_t distance = h_distances[i];
            if (is_unreachable(distance)) {
                continue;
            }
            result.distance.emplace(projection.mapper().global_id(dense),
                                    static_cast<std::uint32_t>(distance));
        }
    }

    template <typename vertex_t, typename edge_t>
    static BfsResult to_external_result(const CsrGraphProjection<vertex_t, edge_t>& projection,
                                        const vertex_t* h_distances,
                                        const vertex_t* h_predecessors,
                                        const std::size_t count,
                                        const bool has_predecessors,
                                        const std::size_t dense_offset) {
        BfsResult result;
        append_external_result(result, projection, h_distances, h_predecessors, count,
                               has_predecessors, dense_offset);
        return result;
    }

    template <typename vertex_t, typename edge_t>
    static void append_external_result(BfsResult& result,
                                       const CsrGraphProjection<vertex_t, edge_t>& projection,
                                       const vertex_t* h_distances,
                                       const vertex_t* h_predecessors,
                                       const std::size_t count,
                                       const bool has_predecessors,
                                       const std::size_t dense_offset) {
        EVENT_SCOPE_INFO(conversion_span, "cugraph.bfs_to_intermediate_conversion",
                         {"dense_vertices", count}, {"dense_offset", dense_offset},
                         {"has_predecessors", has_predecessors});
        for (std::size_t local = 0; local < count; ++local) {
            const vertex_t distance = h_distances[local];
            if (is_unreachable(distance)) {
                continue;
            }
            const auto dense = dense_offset + local;
            const GlobalId& external_id = projection.mapper().global_id(static_cast<vertex_t>(dense));
            result.distance.emplace(external_id, static_cast<std::uint32_t>(distance));
            if (has_predecessors) {
                const vertex_t predecessor = h_predecessors[local];
                if (!is_unreachable(predecessor)) {
                    result.predecessor.emplace(external_id, projection.mapper().global_id(predecessor));
                }
            }
        }
    }

    template <typename vertex_t> static bool is_unreachable(vertex_t value) {
        if constexpr (std::numeric_limits<vertex_t>::is_signed) {
            return value < 0;
        } else {
            return value == std::numeric_limits<vertex_t>::max();
        }
    }
};

} // namespace polygraph

#endif // POLYGRAPH_CUGRAPHBFSEXECUTOR_HPP
