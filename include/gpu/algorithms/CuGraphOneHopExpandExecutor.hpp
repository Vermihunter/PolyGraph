/**
 * @file include/gpu/algorithms/CuGraphOneHopExpandExecutor.hpp
 * @brief Declarations for cu graph one hop expand executor in the GPU acceleration module.
 */

#ifndef POLYGRAPH_CUGRAPH_ONE_HOP_EXPAND_EXECUTOR_HPP
#define POLYGRAPH_CUGRAPH_ONE_HOP_EXPAND_EXECUTOR_HPP

#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "gpu/data_model/CsrGraphProjection.hpp"
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
#include <span>
#include <stdexcept>
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
        EVENT_SCOPE_INFO(total_span, "cugraph.one_hop", {"source_count", source_ids.size()},
                         {"projection_vertices", projection.vertex_count()});
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
                                           Consumer&& consumer) {
        EVENT_SCOPE_INFO(total_span, "cugraph.one_hop.unique_destinations",
                         {"source_count", source_ids.size()},
                         {"projection_vertices", projection.vertex_count()},
                         {"candidate_batch_size", candidate_batch_size});
        if (candidate_batch_size == 0) {
            throw std::invalid_argument("GPU candidate batch size must be greater than zero");
        }
        if (source_ids.empty()) {
            total_span.add_end_field("sampled_edges", 0);
            total_span.add_end_field("unique_destinations", 0);
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
            map_span.add_end_field("dense-source-size", h_dense_sources.size());
        }

        auto &handle = projection.handle();
        auto stream = handle.get_stream();
        auto d_sources = rmm::device_uvector<vertex_t>(h_dense_sources.size(), stream);
        raft::update_device(d_sources.data(), h_dense_sources.data(), h_dense_sources.size(), stream);

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
        total_span.add_end_field("sampled_edges", sampled_edges);
        if (sampled_edges == 0) {
            total_span.add_end_field("unique_destinations", 0);
            return;
        }

        std::vector<vertex_t> h_sampled_dense(sampled_edges);
        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.destinations_d2h",
                             {"sampled_edges", sampled_edges},
                             {"bytes", sampled_edges * sizeof(vertex_t)});
            raft::update_host(h_sampled_dense.data(), d_sampled_dsts.data(), sampled_edges, stream);
            handle.sync_stream();
        }

        std::size_t unique_count = 0;
        {
            // This header is instantiated by ordinary C++ translation units. CUDA Thrust
            // algorithms here would dispatch through RAPIDS/CCCL's generic host-compiler
            // path, which is not implemented. Keep one dense destination ID per sampled
            // edge, copy only that half of the sampled edge list, and deduplicate in-place.
            EVENT_SCOPE_INFO(unique_span, "cugraph.one_hop.destination_sort_unique",
                             {"sampled_edges", sampled_edges}, {"execution", "host"});
            std::sort(h_sampled_dense.begin(), h_sampled_dense.end());
            const auto unique_end = std::unique(h_sampled_dense.begin(), h_sampled_dense.end());
            unique_count = static_cast<std::size_t>(unique_end - h_sampled_dense.begin());
            unique_span.add_end_field("unique_destinations", unique_count);
        }

        std::vector<NodeRef> batch;
        batch.reserve(candidate_batch_size);
        std::size_t emitted_batches = 0;
        const std::span<const vertex_t> unique_destinations{h_sampled_dense.data(), unique_count};
        for (const vertex_t dense: unique_destinations) {
            batch.push_back(NodeRef{.global_id = projection.mapper().global_id(dense),
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

        total_span.add_end_field("unique_destinations", unique_count);
        total_span.add_end_field("emitted_batches", emitted_batches);
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

        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.source_h2d_enqueue",
                             {"source_count", h_dense_sources.size()},
                             {"bytes", h_dense_sources.size() * sizeof(vertex_t)});
            raft::update_device(d_sources.data(), h_dense_sources.data(), h_dense_sources.size(), stream);
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

        std::vector<vertex_t> h_sampled_srcs;
        std::vector<vertex_t> h_sampled_dsts;
        {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.one_hop.host_result_allocate", {"sampled_edges", edge_count},
                             {"bytes", edge_count * sizeof(vertex_t) * 2U});
            h_sampled_srcs.resize(edge_count);
            h_sampled_dsts.resize(edge_count);
        }

        if (edge_count > 0) {
            EVENT_SCOPE_INFO(copy_span, "cugraph.one_hop.result_d2h_enqueue", {"sampled_edges", edge_count},
                             {"bytes", edge_count * sizeof(vertex_t) * 2U});
            raft::update_host(h_sampled_srcs.data(), d_sampled_srcs.data(), edge_count, stream);
            raft::update_host(h_sampled_dsts.data(), d_sampled_dsts.data(), edge_count, stream);
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

        auto result = to_external_result(projection, source_ids, h_sampled_srcs, h_sampled_dsts);
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
                       const std::vector<vertex_t>& h_sampled_srcs,
                       const std::vector<vertex_t>& h_sampled_dsts) {
        EVENT_SCOPE_INFO(external_result_conversion_layer, "cugraph.to-external-result-conversion",
                         {"sampled_edges", h_sampled_srcs.size()});
        OneHopExpansionResult result;
        result.sources = source_ids;
        result.edges.reserve(h_sampled_srcs.size());

        for (std::size_t i = 0; i < h_sampled_srcs.size(); ++i) {
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
