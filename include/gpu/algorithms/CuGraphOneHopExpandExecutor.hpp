/**
 * @file include/gpu/algorithms/CuGraphOneHopExpandExecutor.hpp
 * @brief Declarations for cu graph one hop expand executor in the GPU acceleration module.
 */

#ifndef POLYGRAPH_CUGRAPH_ONE_HOP_EXPAND_EXECUTOR_HPP
#define POLYGRAPH_CUGRAPH_ONE_HOP_EXPAND_EXECUTOR_HPP

#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "gpu/data_model/CsrGraphProjection.hpp"
#include "gpu/algorithms/DeviceCompaction.hpp"
#include "gpu/GpuTransfer.hpp"
#include "gpu/GpuBfsWorkspace.hpp"
#include "data_model/NodeRef.hpp"
#include "events/Events.hpp"

#include <algorithm>
#include <cstdint>
#include <cuda_runtime.h>
#include <cugraph/sampling_functions.hpp>
#include <optional>
#include <raft/core/device_span.hpp>
#include <raft/core/host_span.hpp>
#include <raft/random/rng_state.hpp>
#include <raft/util/cudart_utils.hpp>
#include <rmm/device_uvector.hpp>
#include <rmm/exec_policy.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace polygraph {
struct OneHopExpansionResult {
    std::vector<GlobalId> sources;
    std::vector<std::pair<GlobalId, GlobalId>> edges;

    [[nodiscard]] BfsResult to_union_bfs_result(bool return_predecessors) const {
        BfsResult result;

        for (const auto& source : sources) {
            result.distance.emplace(source, 0);
        }

        for (const auto& [source, destination] : edges) {
            result.distance.emplace(destination, 1);

            if (return_predecessors) {
                // If the same destination is reached from multiple sources,
                // this intentionally keeps the first predecessor only.
                result.predecessor.emplace(destination, source);
            }
        }

        return result;
    }
};

class CuGraphOneHopExpandExecutor {
  public:
    template <typename vertex_t,
              typename edge_t,
              bool MULTI_GPU = false,
              bool STORE_TRANSPOSED = false>
    static OneHopExpansionResult run(CsrGraphProjection<vertex_t, edge_t>& projection,
                                     const std::vector<GlobalId>& source_ids,
                                     const BfsOptions& options) {
        CudaDeviceGuard device_guard(projection.device_id());
        EVENT_SCOPE_INFO(total_span, "cugraph.one_hop", {"source_count", source_ids.size()},
                         {"projection_vertices", projection.vertex_count()},
                         {"transfer_mode", std::string(to_string(projection.execution_config().transfer_mode))});
        validate_options(options);

        OneHopExpansionResult result;
        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.source_result_copy", {"source_count", source_ids.size()});
            result.sources = source_ids;
        }

        if (source_ids.empty()) {
            EVENT_SCOPE_INFO_FIELD(total_span, "sampled_edges", 0);
            return result;
        }

        if (projection.vertex_count() == 0) {
            throw std::runtime_error("GPU projection has zero vertices");
        }

        std::vector<vertex_t> h_dense_sources;
        h_dense_sources.reserve(source_ids.size());
        {
            EVENT_SCOPE_INFO(projection_get_mapper_ids, "cugraph.uuid-to-gpuID", {"source_count", source_ids.size()});
            for (const auto& source_id : source_ids) {
                if (!projection.mapper().contains(source_id)) {
                    throw std::runtime_error("Source vertex is not present in this GPU projection");
                }

                h_dense_sources.push_back(projection.mapper().at(source_id));
            }
            EVENT_SCOPE_INFO_FIELD(projection_get_mapper_ids, "dense-source-size", h_dense_sources.size());
        }

        const std::uint32_t depth = requested_depth(options);

        if (depth == 0) {
            EVENT_SCOPE_INFO_FIELD(total_span, "sampled_edges", 0);
            return result;
        }

        result = run_depth1_sampling<vertex_t, edge_t, MULTI_GPU, STORE_TRANSPOSED>(
            projection, source_ids, h_dense_sources);
        EVENT_SCOPE_INFO_FIELD(total_span, "sampled_edges", result.edges.size());
        return result;
    }

    /**
     * @brief Samples one hop, copies only destination IDs to host, deduplicates
     *        them in-place, and emits bounded candidate batches.
     *
     * This path intentionally drops source IDs and is therefore only suitable
     * when predecessor reconstruction is not requested.
     */
    template <typename vertex_t,
              typename edge_t,
              bool MULTI_GPU = false,
              bool STORE_TRANSPOSED = false,
              typename Consumer>
    static void stream_unique_destinations(CsrGraphProjection<vertex_t, edge_t>& projection,
                                           const std::vector<GlobalId>& source_ids,
                                           std::size_t candidate_batch_size,
                                           Consumer&& consumer,
                                           GpuQueryVisitedWorkspace<vertex_t>* query_workspace = nullptr) {
        CudaDeviceGuard device_guard(projection.device_id());
        EVENT_SCOPE_INFO(total_span, "cugraph.one_hop.unique_destinations",
                         {"source_count", source_ids.size()},
                         {"projection_vertices", projection.vertex_count()},
                         {"candidate_batch_size", candidate_batch_size},
                         {"transfer_mode", std::string(to_string(projection.execution_config().transfer_mode))});
        if (candidate_batch_size == 0) {
            throw std::invalid_argument("GPU candidate batch size must be greater than zero");
        }
        if (source_ids.empty()) {
            EVENT_SCOPE_INFO_FIELD(total_span, "sampled_edges", 0);
            EVENT_SCOPE_INFO_FIELD(total_span, "unique_destinations", 0);
            return;
        }
        if (projection.vertex_count() == 0) {
            throw std::runtime_error("GPU projection has zero vertices");
        }

        std::vector<vertex_t> h_dense_sources;
        h_dense_sources.reserve(source_ids.size());
        {
            EVENT_SCOPE_INFO(map_span, "cugraph.uuid-to-gpuID", {"source_count", source_ids.size()});
            for (const auto &source_id: source_ids) {
                if (!projection.mapper().contains(source_id)) {
                    throw std::runtime_error("Source vertex is not present in this GPU projection");
                }
                h_dense_sources.push_back(projection.mapper().at(source_id));
            }
            EVENT_SCOPE_INFO_FIELD(map_span, "dense-source-size", h_dense_sources.size());
        }

        auto &handle = projection.handle();
        auto stream = handle.get_stream();
        auto d_sources = rmm::device_uvector<vertex_t>(h_dense_sources.size(), stream);
        PinnedHostBuffer<vertex_t> pinned_sources;
        if (projection.execution_config().transfer_mode == GpuTransferMode::Baseline) {
            raft::update_device(d_sources.data(), h_dense_sources.data(), h_dense_sources.size(), stream);
        } else {
            pinned_sources.resize(h_dense_sources.size());
            std::copy(h_dense_sources.begin(), h_dense_sources.end(), pinned_sources.data());
            cuda_check(cudaMemcpyAsync(d_sources.data(), pinned_sources.data(),
                                       h_dense_sources.size() * sizeof(vertex_t), cudaMemcpyHostToDevice, stream),
                       "one-hop pinned sources H2D");
        }

        if (query_workspace != nullptr) {
            EVENT_SCOPE_INFO(visited_source_span, "cugraph.one_hop.query_visited_mark_sources",
                             {"source_count", d_sources.size()});
            gpu_detail::mark_visited_device(d_sources.data(), d_sources.size(),
                                            query_workspace->visited_words.data(), stream);
        }

        raft::device_span<vertex_t const> starting_vertices{d_sources.data(), d_sources.size()};
        std::vector<int32_t> h_fan_out{-1};
        raft::host_span<int32_t const> fan_out{h_fan_out.data(), h_fan_out.size()};
        raft::random::RngState rng_state{0};
        cugraph::sampling_flags_t flags{};
        flags.return_hops = false;
        flags.dedupe_sources = false;
        flags.with_replacement = false;

        auto [d_sampled_srcs,
              d_sampled_dsts,
              d_sampled_weights,
              d_sampled_edge_ids,
              d_sampled_edge_types,
              d_sampled_hops,
              d_sampled_offsets] = execute<vertex_t, edge_t, float, int32_t, MULTI_GPU, STORE_TRANSPOSED>(
                handle, rng_state, projection, starting_vertices, fan_out, flags);
        (void) d_sampled_srcs;
        (void) d_sampled_weights;
        (void) d_sampled_edge_ids;
        (void) d_sampled_edge_types;
        (void) d_sampled_hops;
        (void) d_sampled_offsets;

        const std::size_t sampled_edges = d_sampled_dsts.size();
        EVENT_SCOPE_INFO_FIELD(total_span, "sampled_edges", sampled_edges);
        if (sampled_edges == 0) {
            EVENT_SCOPE_INFO_FIELD(total_span, "unique_destinations", 0);
            return;
        }

        std::size_t unique_count = 0;
        std::size_t emitted_batches = 0;
        const auto &execution = projection.execution_config();

        if (!execution.candidate_deduplication) {
            unique_count = sampled_edges; // emitted count; duplicates are intentionally preserved.
            if (execution.transfer_mode == GpuTransferMode::Pipelined) {
                emitted_batches = pipeline_raw_destinations(projection, d_sampled_dsts, candidate_batch_size,
                                                            execution.d2h_chunk_elements, consumer);
            } else if (execution.transfer_mode == GpuTransferMode::Pinned) {
                PinnedHostBuffer<vertex_t> h_sampled_dense(sampled_edges);
                {
                    EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.destinations_d2h",
                                     {"mode", "pinned+no_dedup"},
                                     {"sampled_edges", sampled_edges},
                                     {"bytes", sampled_edges * sizeof(vertex_t)});
                    cuda_check(cudaMemcpyAsync(h_sampled_dense.data(), d_sampled_dsts.data(),
                                               sampled_edges * sizeof(vertex_t), cudaMemcpyDeviceToHost, stream),
                               "one-hop no-dedup destinations D2H");
                    handle.sync_stream();
                }
                emit_dense_candidates(projection, h_sampled_dense.data(), sampled_edges, candidate_batch_size,
                                      consumer, emitted_batches);
            } else {
                std::vector<vertex_t> h_sampled_dense(sampled_edges);
                {
                    EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.destinations_d2h",
                                     {"mode", "baseline+no_dedup"},
                                     {"sampled_edges", sampled_edges},
                                     {"bytes", sampled_edges * sizeof(vertex_t)});
                    raft::update_host(h_sampled_dense.data(), d_sampled_dsts.data(), sampled_edges, stream);
                    handle.sync_stream();
                }
                emit_dense_candidates(projection, h_sampled_dense.data(), sampled_edges, candidate_batch_size,
                                      consumer, emitted_batches);
            }
        } else if (execution.device_candidate_bitmap_compaction &&
                   execution.transfer_mode != GpuTransferMode::Baseline) {
            const std::size_t bitmap_words = (projection.vertex_count() + 31U) / 32U;
            rmm::device_uvector<std::uint32_t> d_bitmap(bitmap_words, stream);
            rmm::device_uvector<vertex_t> d_unique(sampled_edges, stream);
            rmm::device_uvector<std::uint64_t> d_unique_count(1, stream);

            {
                EVENT_SCOPE_INFO(compact_span, "cugraph.one_hop.destination_device_bitmap_unique",
                                 {"sampled_edges", sampled_edges},
                                 {"bitmap_bytes", bitmap_words * sizeof(std::uint32_t)},
                                 {"execution", "gpu"});
                cuda_check(cudaMemsetAsync(d_bitmap.data(), 0, bitmap_words * sizeof(std::uint32_t), stream),
                           "zero one-hop device dedup bitmap");
                cuda_check(cudaMemsetAsync(d_unique_count.data(), 0, sizeof(std::uint64_t), stream),
                           "zero one-hop device unique counter");
                gpu_detail::bitmap_unique_device(d_sampled_dsts.data(), sampled_edges, d_bitmap.data(),
                                                 d_unique.data(), d_unique_count.data(), stream);

                std::uint64_t h_unique_count = 0;
                cuda_check(cudaMemcpyAsync(&h_unique_count, d_unique_count.data(), sizeof(h_unique_count),
                                           cudaMemcpyDeviceToHost, stream),
                           "one-hop device bitmap unique count D2H");
                handle.sync_stream();
                unique_count = static_cast<std::size_t>(h_unique_count);
                EVENT_SCOPE_INFO_FIELD(compact_span, "unique_destinations", unique_count);
            }

            if (execution.transfer_mode == GpuTransferMode::Pipelined) {
                emitted_batches = pipeline_dense_candidates(projection, d_unique, unique_count,
                                                            candidate_batch_size, execution.d2h_chunk_elements,
                                                            consumer);
            } else {
                PinnedHostBuffer<vertex_t> h_unique(unique_count);
                {
                    EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.destinations_d2h",
                                     {"mode", "device_bitmap+pinned"},
                                     {"sampled_edges", sampled_edges},
                                     {"unique_destinations", unique_count},
                                     {"bytes", unique_count * sizeof(vertex_t)});
                    cuda_check(cudaMemcpyAsync(h_unique.data(), d_unique.data(),
                                               unique_count * sizeof(vertex_t), cudaMemcpyDeviceToHost, stream),
                               "one-hop bitmap-compacted destinations D2H");
                    handle.sync_stream();
                }
                emit_dense_candidates(projection, h_unique.data(), unique_count, candidate_batch_size,
                                      consumer, emitted_batches);
            }
        } else if (execution.device_candidate_compaction && execution.transfer_mode != GpuTransferMode::Baseline) {
            {
                EVENT_SCOPE_INFO(compact_span, "cugraph.one_hop.destination_device_sort_unique",
                                 {"sampled_edges", sampled_edges}, {"execution", "gpu"});
                unique_count = gpu_detail::sort_unique_device(
                    d_sampled_dsts.data(), sampled_edges, stream);
                EVENT_SCOPE_INFO_FIELD(compact_span, "unique_destinations", unique_count);
            }

            rmm::device_uvector<vertex_t>* d_emit = &d_sampled_dsts;
            if (query_workspace != nullptr) {
                std::uint64_t h_new_count = 0;
                {
                    EVENT_SCOPE_INFO(visited_span, "cugraph.one_hop.query_visited_filter",
                                     {"unique_candidates", unique_count});
                    cuda_check(cudaMemsetAsync(query_workspace->filtered_count.data(), 0,
                                               sizeof(std::uint64_t), stream),
                               "reset query visited filtered counter");
                    gpu_detail::filter_unvisited_device(
                            d_sampled_dsts.data(), unique_count,
                            query_workspace->visited_words.data(),
                            query_workspace->filtered_candidates.data(),
                            query_workspace->filtered_count.data(), stream);
                    cuda_check(cudaMemcpyAsync(&h_new_count, query_workspace->filtered_count.data(),
                                               sizeof(h_new_count), cudaMemcpyDeviceToHost, stream),
                               "query visited filtered count D2H");
                    handle.sync_stream();
                    EVENT_SCOPE_INFO_FIELD(visited_span, "new_candidates", h_new_count);
                }
                unique_count = static_cast<std::size_t>(h_new_count);
                d_emit = &query_workspace->filtered_candidates;
            }

            if (execution.transfer_mode == GpuTransferMode::Pipelined) {
                emitted_batches = pipeline_dense_candidates(projection, *d_emit, unique_count,
                                                            candidate_batch_size, execution.d2h_chunk_elements,
                                                            consumer);
            } else {
                PinnedHostBuffer<vertex_t> h_unique(unique_count);
                {
                    EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.destinations_d2h",
                                     {"mode", query_workspace != nullptr
                                                      ? "device_sort_unique+query_visited+pinned"
                                                      : "device_sort_unique+pinned"},
                                     {"sampled_edges", sampled_edges},
                                     {"unique_destinations", unique_count},
                                     {"bytes", unique_count * sizeof(vertex_t)});
                    cuda_check(cudaMemcpyAsync(h_unique.data(), d_emit->data(),
                                               unique_count * sizeof(vertex_t), cudaMemcpyDeviceToHost, stream),
                               "one-hop compacted destinations D2H");
                    handle.sync_stream();
                }
                emit_dense_candidates(projection, h_unique.data(), unique_count, candidate_batch_size,
                                      consumer, emitted_batches);
            }
        } else if (execution.transfer_mode == GpuTransferMode::Baseline) {
            std::vector<vertex_t> h_sampled_dense(sampled_edges);
            {
                EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.destinations_d2h",
                                 {"mode", "baseline"},
                                 {"sampled_edges", sampled_edges},
                                 {"bytes", sampled_edges * sizeof(vertex_t)});
                raft::update_host(h_sampled_dense.data(), d_sampled_dsts.data(), sampled_edges, stream);
                handle.sync_stream();
            }

            {
                EVENT_SCOPE_INFO(unique_span, "cugraph.one_hop.destination_sort_unique",
                                 {"sampled_edges", sampled_edges}, {"execution", "host"});
                std::sort(h_sampled_dense.begin(), h_sampled_dense.end());
                const auto unique_end = std::unique(h_sampled_dense.begin(), h_sampled_dense.end());
                unique_count = static_cast<std::size_t>(unique_end - h_sampled_dense.begin());
            }
            emit_dense_candidates(projection, h_sampled_dense.data(), unique_count, candidate_batch_size,
                                  consumer, emitted_batches);
        } else if (execution.transfer_mode == GpuTransferMode::Pinned) {
            PinnedHostBuffer<vertex_t> h_sampled_dense(sampled_edges);
            {
                EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.destinations_d2h",
                                 {"mode", "pinned"},
                                 {"sampled_edges", sampled_edges},
                                 {"bytes", sampled_edges * sizeof(vertex_t)});
                cuda_check(cudaMemcpyAsync(h_sampled_dense.data(), d_sampled_dsts.data(),
                                           sampled_edges * sizeof(vertex_t), cudaMemcpyDeviceToHost, stream),
                           "one-hop pinned destinations D2H");
                handle.sync_stream();
            }
            if (execution.dense_candidate_deduplication) {
                DenseVisitBitmap seen(projection.vertex_count());
                emit_bitmap_deduplicated(projection, h_sampled_dense.data(), sampled_edges,
                                         candidate_batch_size, consumer, seen, unique_count, emitted_batches);
            } else {
                std::sort(h_sampled_dense.data(), h_sampled_dense.data() + sampled_edges);
                const auto unique_end = std::unique(h_sampled_dense.data(), h_sampled_dense.data() + sampled_edges);
                unique_count = static_cast<std::size_t>(unique_end - h_sampled_dense.data());
                emit_dense_candidates(projection, h_sampled_dense.data(), unique_count, candidate_batch_size,
                                      consumer, emitted_batches);
            }
        } else {
            std::tie(unique_count, emitted_batches) = pipeline_unique_destinations(
                projection, d_sampled_dsts, candidate_batch_size, execution.d2h_chunk_elements, consumer);
        }

        EVENT_SCOPE_INFO_FIELD(total_span, "unique_destinations", unique_count);
        EVENT_SCOPE_INFO_FIELD(total_span, "emitted_batches", emitted_batches);
    }

    template <typename vertex_t,
              typename edge_t,
              bool MULTI_GPU = false,
              bool STORE_TRANSPOSED = false>
    static BfsResult run_as_union_bfs_result(CsrGraphProjection<vertex_t, edge_t>& projection,
                                             const std::vector<GlobalId>& source_ids,
                                             const BfsOptions& options) {
        EVENT_SCOPE_INFO(convert_span, "cugraph.one_hop.to_union_bfs_result", {"source_count", source_ids.size()});
        OneHopExpansionResult expanded =
            run<vertex_t, edge_t, MULTI_GPU, STORE_TRANSPOSED>(projection, source_ids, options);
        auto result = expanded.to_union_bfs_result(options.return_predecessors);
        EVENT_SCOPE_INFO_FIELD(convert_span, "sampled_edges", expanded.edges.size());
        EVENT_SCOPE_INFO_FIELD(convert_span, "result_vertices", result.distance.size());
        return result;
    }

  private:
    template <typename vertex_t, typename edge_t, typename Consumer>
    static void emit_dense_candidates(const CsrGraphProjection<vertex_t, edge_t>& projection,
                                      const vertex_t* dense_values,
                                      const std::size_t count,
                                      const std::size_t candidate_batch_size,
                                      Consumer& consumer,
                                      std::size_t& emitted_batches) {
        std::vector<NodeRef> batch;
        batch.reserve(candidate_batch_size);
        for (std::size_t i = 0; i < count; ++i) {
            batch.push_back(NodeRef{.global_id = projection.mapper().global_id(dense_values[i]),
                                    .type = unresolved_type_id});
            if (batch.size() >= candidate_batch_size) {
                consumer(std::span<const NodeRef>(batch));
                batch.clear();
                ++emitted_batches;
            }
        }
        if (!batch.empty()) {
            consumer(std::span<const NodeRef>(batch));
            ++emitted_batches;
        }
    }

    template <typename vertex_t, typename edge_t, typename Consumer>
    static void emit_bitmap_deduplicated(const CsrGraphProjection<vertex_t, edge_t>& projection,
                                         const vertex_t* dense_values,
                                         const std::size_t count,
                                         const std::size_t candidate_batch_size,
                                         Consumer& consumer,
                                         DenseVisitBitmap& seen,
                                         std::size_t& unique_count,
                                         std::size_t& emitted_batches,
                                         std::vector<NodeRef>* reusable_batch = nullptr) {
        std::vector<NodeRef> owned_batch;
        std::vector<NodeRef>& batch = reusable_batch ? *reusable_batch : owned_batch;
        if (batch.capacity() < candidate_batch_size) {
            batch.reserve(candidate_batch_size);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto dense = static_cast<std::size_t>(dense_values[i]);
            if (seen.test_and_set(dense)) {
                continue;
            }
            ++unique_count;
            batch.push_back(NodeRef{.global_id = projection.mapper().global_id(dense_values[i]),
                                    .type = unresolved_type_id});
            if (batch.size() >= candidate_batch_size) {
                consumer(std::span<const NodeRef>(batch));
                batch.clear();
                ++emitted_batches;
            }
        }
        if (!reusable_batch && !batch.empty()) {
            consumer(std::span<const NodeRef>(batch));
            batch.clear();
            ++emitted_batches;
        }
    }

    template <typename vertex_t, typename edge_t, typename Consumer>
    static std::size_t pipeline_dense_candidates(CsrGraphProjection<vertex_t, edge_t>& projection,
                                                 const rmm::device_uvector<vertex_t>& d_values,
                                                 const std::size_t value_count,
                                                 const std::size_t candidate_batch_size,
                                                 const std::size_t requested_chunk,
                                                 Consumer& consumer) {
        EVENT_SCOPE_INFO(pipe_span, "cugraph.one_hop.compacted_destinations_d2h_pipeline",
                         {"unique_destinations", value_count}, {"chunk_elements", requested_chunk});
        if (value_count == 0) {
            return 0;
        }
        const std::size_t chunk = std::max<std::size_t>(1, std::min(requested_chunk, value_count));
        auto compute_stream = projection.handle().get_stream();

        CudaEvent compact_done;
        cuda_check(cudaEventRecord(compact_done.get(), compute_stream), "record one-hop compaction completion");
        CudaStream copy_streams[2];
        CudaEvent ready[2];
        for (auto& copy_stream : copy_streams) {
            cuda_check(cudaStreamWaitEvent(copy_stream.get(), compact_done.get(), 0),
                       "one-hop compacted copy stream wait");
        }
        PinnedHostBuffer<vertex_t> slots[2]{PinnedHostBuffer<vertex_t>(chunk),
                                            PinnedHostBuffer<vertex_t>(chunk)};
        struct PendingChunk { std::size_t count{0}; bool valid{false}; };
        PendingChunk pending[2];
        std::size_t emitted_batches = 0;

        auto consume = [&](const int slot) {
            if (!pending[slot].valid) {
                return;
            }
            cuda_check(cudaEventSynchronize(ready[slot].get()), "wait compacted one-hop D2H chunk");
            emit_dense_candidates(projection, slots[slot].data(), pending[slot].count,
                                  candidate_batch_size, consumer, emitted_batches);
            pending[slot].valid = false;
        };

        std::size_t chunk_index = 0;
        for (std::size_t offset = 0; offset < value_count; offset += chunk, ++chunk_index) {
            const int slot = static_cast<int>(chunk_index & 1U);
            consume(slot);
            const std::size_t count = std::min(chunk, value_count - offset);
            const auto copy_stream = copy_streams[slot].get();
            cuda_check(cudaMemcpyAsync(slots[slot].data(), d_values.data() + offset,
                                       count * sizeof(vertex_t), cudaMemcpyDeviceToHost, copy_stream),
                       "one-hop compacted pipelined destinations D2H");
            cuda_check(cudaEventRecord(ready[slot].get(), copy_stream),
                       "record compacted one-hop D2H chunk");
            pending[slot] = PendingChunk{count, true};
        }
        consume(0);
        consume(1);
        EVENT_SCOPE_INFO_FIELD(pipe_span, "emitted_batches", emitted_batches);
        return emitted_batches;
    }

    template <typename vertex_t, typename edge_t, typename Consumer>
    static std::size_t pipeline_raw_destinations(CsrGraphProjection<vertex_t, edge_t>& projection,
                                                 const rmm::device_uvector<vertex_t>& d_destinations,
                                                 const std::size_t candidate_batch_size,
                                                 const std::size_t requested_chunk,
                                                 Consumer& consumer) {
        EVENT_SCOPE_INFO(pipe_span, "cugraph.one_hop.destinations_d2h_pipeline_no_dedup",
                         {"sampled_edges", d_destinations.size()},
                         {"chunk_elements", requested_chunk});
        const std::size_t total = d_destinations.size();
        if (total == 0) {
            return 0;
        }
        const std::size_t chunk = std::max<std::size_t>(1, std::min(requested_chunk, total));
        auto compute_stream = projection.handle().get_stream();

        CudaEvent sampling_done;
        cuda_check(cudaEventRecord(sampling_done.get(), compute_stream), "record one-hop sampling completion");
        CudaStream copy_streams[2];
        CudaEvent ready[2];
        for (auto& copy_stream : copy_streams) {
            cuda_check(cudaStreamWaitEvent(copy_stream.get(), sampling_done.get(), 0),
                       "one-hop no-dedup copy stream wait");
        }
        PinnedHostBuffer<vertex_t> slots[2]{PinnedHostBuffer<vertex_t>(chunk),
                                            PinnedHostBuffer<vertex_t>(chunk)};
        struct PendingChunk { std::size_t count{0}; bool valid{false}; };
        PendingChunk pending[2];
        std::size_t emitted_batches = 0;

        auto consume = [&](const int slot) {
            if (!pending[slot].valid) {
                return;
            }
            cuda_check(cudaEventSynchronize(ready[slot].get()), "wait no-dedup one-hop D2H chunk");
            emit_dense_candidates(projection, slots[slot].data(), pending[slot].count,
                                  candidate_batch_size, consumer, emitted_batches);
            pending[slot].valid = false;
        };

        std::size_t chunk_index = 0;
        for (std::size_t offset = 0; offset < total; offset += chunk, ++chunk_index) {
            const int slot = static_cast<int>(chunk_index & 1U);
            consume(slot);
            const std::size_t count = std::min(chunk, total - offset);
            const auto copy_stream = copy_streams[slot].get();
            cuda_check(cudaMemcpyAsync(slots[slot].data(), d_destinations.data() + offset,
                                       count * sizeof(vertex_t), cudaMemcpyDeviceToHost, copy_stream),
                       "one-hop no-dedup pipelined destinations D2H");
            cuda_check(cudaEventRecord(ready[slot].get(), copy_stream),
                       "record no-dedup one-hop D2H chunk");
            pending[slot] = PendingChunk{count, true};
        }
        consume(0);
        consume(1);
        EVENT_SCOPE_INFO_FIELD(pipe_span, "emitted_candidates", total);
        EVENT_SCOPE_INFO_FIELD(pipe_span, "emitted_batches", emitted_batches);
        return emitted_batches;
    }

    template <typename vertex_t, typename edge_t, typename Consumer>
    static std::pair<std::size_t, std::size_t>
    pipeline_unique_destinations(CsrGraphProjection<vertex_t, edge_t>& projection,
                                 const rmm::device_uvector<vertex_t>& d_destinations,
                                 const std::size_t candidate_batch_size,
                                 const std::size_t requested_chunk,
                                 Consumer& consumer) {
        EVENT_SCOPE_INFO(pipe_span, "cugraph.one_hop.destinations_d2h_pipeline",
                         {"sampled_edges", d_destinations.size()},
                         {"chunk_elements", requested_chunk});
        const std::size_t total = d_destinations.size();
        const std::size_t chunk = std::max<std::size_t>(1, std::min(requested_chunk, total));
        auto compute_stream = projection.handle().get_stream();

        CudaEvent sampling_done;
        cuda_check(cudaEventRecord(sampling_done.get(), compute_stream), "record one-hop sampling completion");
        CudaStream copy_streams[2];
        CudaEvent ready[2];
        for (auto& copy_stream : copy_streams) {
            cuda_check(cudaStreamWaitEvent(copy_stream.get(), sampling_done.get(), 0),
                       "one-hop copy stream wait");
        }
        PinnedHostBuffer<vertex_t> slots[2]{PinnedHostBuffer<vertex_t>(chunk),
                                            PinnedHostBuffer<vertex_t>(chunk)};
        struct PendingChunk { std::size_t count{0}; bool valid{false}; };
        PendingChunk pending[2];

        DenseVisitBitmap seen(projection.vertex_count());
        std::vector<NodeRef> batch;
        batch.reserve(candidate_batch_size);
        std::size_t unique_count = 0;
        std::size_t emitted_batches = 0;

        auto consume = [&](const int slot) {
            if (!pending[slot].valid) {
                return;
            }
            cuda_check(cudaEventSynchronize(ready[slot].get()), "wait one-hop D2H chunk");
            emit_bitmap_deduplicated(projection, slots[slot].data(), pending[slot].count,
                                     candidate_batch_size, consumer, seen, unique_count, emitted_batches, &batch);
            pending[slot].valid = false;
        };

        std::size_t chunk_index = 0;
        for (std::size_t offset = 0; offset < total; offset += chunk, ++chunk_index) {
            const int slot = static_cast<int>(chunk_index & 1U);
            consume(slot);
            const std::size_t count = std::min(chunk, total - offset);
            const auto copy_stream = copy_streams[slot].get();
            cuda_check(cudaMemcpyAsync(slots[slot].data(), d_destinations.data() + offset,
                                       count * sizeof(vertex_t), cudaMemcpyDeviceToHost, copy_stream),
                       "one-hop pipelined destinations D2H");
            cuda_check(cudaEventRecord(ready[slot].get(), copy_stream), "record one-hop D2H chunk");
            pending[slot] = PendingChunk{count, true};
        }
        consume(0);
        consume(1);
        if (!batch.empty()) {
            consumer(std::span<const NodeRef>(batch));
            ++emitted_batches;
        }
        EVENT_SCOPE_INFO_FIELD(pipe_span, "unique_destinations", unique_count);
        EVENT_SCOPE_INFO_FIELD(pipe_span, "bitmap_bytes", seen.bytes());
        EVENT_SCOPE_INFO_FIELD(pipe_span, "emitted_batches", emitted_batches);
        return {unique_count, emitted_batches};
    }

    struct GpuPhaseTiming {
        float source_h2d_ms{0.0F};
        float sampling_ms{0.0F};
        float result_d2h_ms{0.0F};
        float total_stream_ms{0.0F};
        bool valid{false};
    };

    class GpuPhaseTimer {
      public:
        explicit GpuPhaseTimer(const bool enabled) : enabled_(enabled) {
            if (!enabled_) {
                return;
            }
            if (cudaEventCreate(&start_) != cudaSuccess ||
                cudaEventCreate(&after_h2d_) != cudaSuccess ||
                cudaEventCreate(&after_sampling_) != cudaSuccess ||
                cudaEventCreate(&after_d2h_) != cudaSuccess) {
                cleanup();
                enabled_ = false;
            }
        }

        GpuPhaseTimer(const GpuPhaseTimer&) = delete;
        GpuPhaseTimer& operator=(const GpuPhaseTimer&) = delete;

        ~GpuPhaseTimer() { cleanup(); }

        template <typename StreamT> void record_start(StreamT stream) { record(start_, stream); }
        template <typename StreamT> void record_after_h2d(StreamT stream) { record(after_h2d_, stream); }
        template <typename StreamT> void record_after_sampling(StreamT stream) { record(after_sampling_, stream); }
        template <typename StreamT> void record_after_d2h(StreamT stream) { record(after_d2h_, stream); }

        [[nodiscard]] GpuPhaseTiming elapsed() const noexcept {
            GpuPhaseTiming timing{};
            if (!enabled_) {
                return timing;
            }

            if (cudaEventElapsedTime(&timing.source_h2d_ms, start_, after_h2d_) != cudaSuccess ||
                cudaEventElapsedTime(&timing.sampling_ms, after_h2d_, after_sampling_) != cudaSuccess ||
                cudaEventElapsedTime(&timing.result_d2h_ms, after_sampling_, after_d2h_) != cudaSuccess ||
                cudaEventElapsedTime(&timing.total_stream_ms, start_, after_d2h_) != cudaSuccess) {
                return {};
            }
            timing.valid = true;
            return timing;
        }

      private:
        template <typename StreamT> void record(cudaEvent_t event, StreamT stream) {
            if (enabled_ && cudaEventRecord(event, stream) != cudaSuccess) {
                enabled_ = false;
            }
        }

        void cleanup() noexcept {
            if (start_ != nullptr)
                cudaEventDestroy(start_);
            if (after_h2d_ != nullptr)
                cudaEventDestroy(after_h2d_);
            if (after_sampling_ != nullptr)
                cudaEventDestroy(after_sampling_);
            if (after_d2h_ != nullptr)
                cudaEventDestroy(after_d2h_);
            start_ = nullptr;
            after_h2d_ = nullptr;
            after_sampling_ = nullptr;
            after_d2h_ = nullptr;
        }

        bool enabled_{false};
        cudaEvent_t start_{nullptr};
        cudaEvent_t after_h2d_{nullptr};
        cudaEvent_t after_sampling_{nullptr};
        cudaEvent_t after_d2h_{nullptr};
    };

    static std::uint32_t requested_depth(const BfsOptions& options) {
        if (!options.max_depth.has_value()) {
            return 1;
        }

        return static_cast<std::uint32_t>(*options.max_depth);
    }

    static void validate_options(const BfsOptions& options) {
        const std::uint32_t depth = requested_depth(options);

        if (depth > 1) {
            throw std::runtime_error(
                "CuGraphOneHopExpandExecutor supports only max_depth <= 1. "
                "Use real BFS or iterative frontier expansion for deeper traversal.");
        }
    }

    template <typename vertex_t, typename edge_t, bool MULTI_GPU, bool STORE_TRANSPOSED>
    static OneHopExpansionResult
    run_depth1_sampling(CsrGraphProjection<vertex_t, edge_t>& projection,
                        const std::vector<GlobalId>& source_ids,
                        const std::vector<vertex_t>& h_dense_sources) {
        using weight_t = float;
        using edge_type_t = int32_t;



        EVENT_SCOPE_INFO(total_span, "cugraph.one_hop.depth1", {"dense_sources", h_dense_sources.size()});
        auto& handle = projection.handle();
        auto stream = handle.get_stream();

        const bool gpu_timing_enabled =
            EVENT_ENABLED(EventLevel::Info, "cugraph.one_hop.gpu_phase_timing");
        GpuPhaseTimer gpu_timer(gpu_timing_enabled);
        gpu_timer.record_start(stream);

        auto d_sources = [&]() {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.one_hop.device_source_allocate",
                             {"source_count", h_dense_sources.size()},
                             {"bytes", h_dense_sources.size() * sizeof(vertex_t)});
            return rmm::device_uvector<vertex_t>(h_dense_sources.size(), stream);
        }();

        PinnedHostBuffer<vertex_t> pinned_sources;
        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.source_h2d_enqueue",
                             {"mode", projection.execution_config().transfer_mode == GpuTransferMode::Baseline
                                          ? "baseline" : "pinned"},
                             {"source_count", h_dense_sources.size()},
                             {"bytes", h_dense_sources.size() * sizeof(vertex_t)});
            if (projection.execution_config().transfer_mode == GpuTransferMode::Baseline) {
                raft::update_device(d_sources.data(), h_dense_sources.data(), h_dense_sources.size(), stream);
            } else {
                pinned_sources.resize(h_dense_sources.size());
                std::copy(h_dense_sources.begin(), h_dense_sources.end(), pinned_sources.data());
                cuda_check(cudaMemcpyAsync(d_sources.data(), pinned_sources.data(),
                                           h_dense_sources.size() * sizeof(vertex_t), cudaMemcpyHostToDevice, stream),
                           "one-hop pinned sources H2D");
            }
        }
        gpu_timer.record_after_h2d(stream);

        raft::device_span<vertex_t const> starting_vertices{d_sources.data(), d_sources.size()};

        // One hop, all neighbors. In cuGraph sampling, a negative fanout means
        // "select all neighbors".
        std::vector<int32_t> h_fan_out{-1};
        raft::host_span<int32_t const> fan_out{h_fan_out.data(), h_fan_out.size()};
        raft::random::RngState rng_state{0};

        cugraph::sampling_flags_t flags{};
        flags.return_hops = false;
        flags.dedupe_sources = false;
        flags.with_replacement = false;

        auto [d_sampled_srcs,
              d_sampled_dsts,
              d_sampled_weights,
              d_sampled_edge_ids,
              d_sampled_edge_types,
              d_sampled_hops,
              d_sampled_offsets] = execute<vertex_t, edge_t, weight_t, edge_type_t, MULTI_GPU, STORE_TRANSPOSED>(
              handle, rng_state, projection, starting_vertices, fan_out, flags);
        gpu_timer.record_after_sampling(stream);

        const std::size_t edge_count = d_sampled_srcs.size();
        EVENT_SCOPE_INFO_FIELD(total_span, "sampled_edges", edge_count);

        const bool use_pinned = projection.execution_config().transfer_mode != GpuTransferMode::Baseline;
        std::vector<vertex_t> pageable_srcs;
        std::vector<vertex_t> pageable_dsts;
        PinnedHostBuffer<vertex_t> pinned_srcs;
        PinnedHostBuffer<vertex_t> pinned_dsts;

        if (use_pinned) {
            pinned_srcs.resize(edge_count);
            pinned_dsts.resize(edge_count);
        } else {
            pageable_srcs.resize(edge_count);
            pageable_dsts.resize(edge_count);
        }

        if (edge_count > 0) {
            EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.result_d2h_enqueue",
                             {"mode", use_pinned ? "pinned" : "baseline"},
                             {"sampled_edges", edge_count},
                             {"bytes", edge_count * sizeof(vertex_t) * 2U});
            if (use_pinned) {
                cuda_check(cudaMemcpyAsync(pinned_srcs.data(), d_sampled_srcs.data(), edge_count * sizeof(vertex_t),
                                           cudaMemcpyDeviceToHost, stream),
                           "one-hop sampled sources D2H");
                cuda_check(cudaMemcpyAsync(pinned_dsts.data(), d_sampled_dsts.data(), edge_count * sizeof(vertex_t),
                                           cudaMemcpyDeviceToHost, stream),
                           "one-hop sampled destinations D2H");
            } else {
                raft::update_host(pageable_srcs.data(), d_sampled_srcs.data(), edge_count, stream);
                raft::update_host(pageable_dsts.data(), d_sampled_dsts.data(), edge_count, stream);
            }
        }
        gpu_timer.record_after_d2h(stream);

        {
            EVENT_SCOPE_INFO(wait_span, "cugraph.one_hop.stream_wait", {"sampled_edges", edge_count});
            handle.sync_stream();
            const auto timing = gpu_timer.elapsed();
            if (timing.valid) {
                EVENT_SCOPE_INFO_FIELD(wait_span, "gpu_source_h2d_ms", timing.source_h2d_ms);
                EVENT_SCOPE_INFO_FIELD(wait_span, "gpu_sampling_ms", timing.sampling_ms);
                EVENT_SCOPE_INFO_FIELD(wait_span, "gpu_result_d2h_ms", timing.result_d2h_ms);
                EVENT_SCOPE_INFO_FIELD(wait_span, "gpu_total_stream_ms", timing.total_stream_ms);
                EVENT_SCOPE_INFO_FIELD(total_span, "gpu_source_h2d_ms", timing.source_h2d_ms);
                EVENT_SCOPE_INFO_FIELD(total_span, "gpu_sampling_ms", timing.sampling_ms);
                EVENT_SCOPE_INFO_FIELD(total_span, "gpu_result_d2h_ms", timing.result_d2h_ms);
                EVENT_SCOPE_INFO_FIELD(total_span, "gpu_total_stream_ms", timing.total_stream_ms);
                EVENT_INFO("cugraph.one_hop.gpu_phase_timing",
                           {"source_h2d_ms", timing.source_h2d_ms},
                           {"sampling_ms", timing.sampling_ms},
                           {"result_d2h_ms", timing.result_d2h_ms},
                           {"total_stream_ms", timing.total_stream_ms},
                           {"sampled_edges", edge_count});
            }
        }

        const vertex_t* h_src_ptr = use_pinned ? pinned_srcs.data() : pageable_srcs.data();
        const vertex_t* h_dst_ptr = use_pinned ? pinned_dsts.data() : pageable_dsts.data();
        auto result = to_external_result(projection, source_ids, h_src_ptr, h_dst_ptr, edge_count);
        EVENT_SCOPE_INFO_FIELD(total_span, "external_edges", result.edges.size());
        return result;
    }

    template <typename vertex_t,
              typename edge_t,
              typename weight_t,
              typename edge_type_t,
              bool MULTI_GPU,
              bool STORE_TRANSPOSED>
    static auto execute(raft::handle_t& handle,
                        raft::random::RngState& rng_state,
                        CsrGraphProjection<vertex_t, edge_t>& projection,
                        raft::device_span<vertex_t const>& starting_vertices,
                        raft::host_span<int32_t const>& fan_out,
                        cugraph::sampling_flags_t& flags) {
        // This scope measures the host-side cuGraph API call. The actual GPU time
        // is measured with CUDA events and attached to cugraph.one_hop.stream_wait.
        EVENT_SCOPE_INFO(cugraph_api, "cugraph.sample.api_call",
                         {"starting_vertices", starting_vertices.size()}, {"fanout_levels", fan_out.size()});

        auto result = cugraph::homogeneous_uniform_neighbor_sample<vertex_t,
                                                                   edge_t,
                                                                   weight_t,
                                                                   edge_type_t,
                                                                   STORE_TRANSPOSED,
                                                                   MULTI_GPU>(
            handle,
            rng_state,
            projection.view(),
            std::nullopt, // edge weights
            std::nullopt, // edge IDs
            std::nullopt, // edge types
            starting_vertices,
            std::nullopt, // starting vertex labels
            std::nullopt, // label_to_output_comm_rank
            fan_out,
            flags,
            false // do_expensive_check
        );
        EVENT_SCOPE_INFO_FIELD(cugraph_api, "sampled_edges", std::get<0>(result).size());
        return result;
    }

    template <typename vertex_t, typename edge_t>
    static OneHopExpansionResult
    to_external_result(const CsrGraphProjection<vertex_t, edge_t>& projection,
                       const std::vector<GlobalId>& source_ids,
                       const vertex_t* h_sampled_srcs,
                       const vertex_t* h_sampled_dsts,
                       const std::size_t edge_count) {
        EVENT_SCOPE_INFO(external_result_conversion_layer, "cugraph.to-external-result-conversion",
                         {"sampled_edges", edge_count});
        OneHopExpansionResult result;
        result.sources = source_ids;
        result.edges.reserve(edge_count);

        for (std::size_t i = 0; i < edge_count; ++i) {
            const GlobalId& external_source = projection.mapper().global_id(h_sampled_srcs[i]);
            const GlobalId& external_destination = projection.mapper().global_id(h_sampled_dsts[i]);
            result.edges.emplace_back(external_source, external_destination);
        }

        EVENT_SCOPE_INFO_FIELD(external_result_conversion_layer, "external_edges", result.edges.size());
        return result;
    }
};

} // namespace polygraph

#endif // POLYGRAPH_CUGRAPH_ONE_HOP_EXPAND_EXECUTOR_HPP
