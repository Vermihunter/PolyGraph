/**
 * @file include/gpu/algorithms/CuGraphOneHopExpandExecutor.hpp
 * @brief Declarations for cu graph one hop expand executor in the GPU acceleration module.
 */

#ifndef CPGDB_CUGRAPH_ONE_HOP_EXPAND_EXECUTOR_HPP
#define CPGDB_CUGRAPH_ONE_HOP_EXPAND_EXECUTOR_HPP

#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "gpu/data_model/CsrGraphProjection.hpp"

#include <cstdint>
#include <cugraph/sampling_functions.hpp>
#include <optional>
#include <raft/core/device_span.hpp>
#include <raft/core/host_span.hpp>
#include <raft/random/rng_state.hpp>
#include <raft/util/cudart_utils.hpp>
#include <rmm/device_uvector.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpgdb {
struct OneHopExpansionResult {
    std::vector<GlobalId> sources;
    std::vector<std::pair<GlobalId, GlobalId>> edges;

    BfsResult to_union_bfs_result(bool return_predecessors) const {
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
        validate_options(options);

        OneHopExpansionResult result;
        result.sources = source_ids;

        if (source_ids.empty()) {
            return result;
        }

        if (projection.vertex_count() == 0) {
            throw std::runtime_error("GPU projection has zero vertices");
        }

        std::vector<vertex_t> h_dense_sources;
        h_dense_sources.reserve(source_ids.size());

        for (const auto& source_id : source_ids) {
            if (!projection.mapper().contains(source_id)) {
                throw std::runtime_error("Source vertex is not present in this GPU projection");
            }

            h_dense_sources.push_back(projection.mapper().at(source_id));
        }

        const std::uint32_t depth = requested_depth(options);

        if (depth == 0) {
            return result;
        }

        return run_depth1_sampling<vertex_t, edge_t, MULTI_GPU, STORE_TRANSPOSED>(
            projection, source_ids, h_dense_sources);
    }

    template <typename vertex_t,
              typename edge_t,
              bool MULTI_GPU = false,
              bool STORE_TRANSPOSED = false>
    static BfsResult run_as_union_bfs_result(CsrGraphProjection<vertex_t, edge_t>& projection,
                                             const std::vector<GlobalId>& source_ids,
                                             const BfsOptions& options) {
        OneHopExpansionResult expanded =
            run<vertex_t, edge_t, MULTI_GPU, STORE_TRANSPOSED>(projection, source_ids, options);

        return expanded.to_union_bfs_result(options.return_predecessors);
    }

  private:
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

        auto& handle = projection.handle();
        auto stream = handle.get_stream();

        rmm::device_uvector<vertex_t> d_sources(h_dense_sources.size(), stream);

        raft::update_device(
            d_sources.data(), h_dense_sources.data(), h_dense_sources.size(), stream);

        raft::device_span<vertex_t const> starting_vertices{d_sources.data(), d_sources.size()};

        // One hop, all neighbors.
        // In cuGraph sampling, a negative fanout means "select all neighbors".
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
              d_sampled_offsets] = cugraph::homogeneous_uniform_neighbor_sample<vertex_t,
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

        const std::size_t edge_count = d_sampled_srcs.size();

        std::vector<vertex_t> h_sampled_srcs(edge_count);
        std::vector<vertex_t> h_sampled_dsts(edge_count);

        if (edge_count > 0) {
            raft::update_host(h_sampled_srcs.data(), d_sampled_srcs.data(), edge_count, stream);

            raft::update_host(h_sampled_dsts.data(), d_sampled_dsts.data(), edge_count, stream);
        }

        handle.sync_stream();

        return to_external_result(projection, source_ids, h_sampled_srcs, h_sampled_dsts);
    }

    template <typename vertex_t, typename edge_t>
    static OneHopExpansionResult
    to_external_result(const CsrGraphProjection<vertex_t, edge_t>& projection,
                       const std::vector<GlobalId>& source_ids,
                       const std::vector<vertex_t>& h_sampled_srcs,
                       const std::vector<vertex_t>& h_sampled_dsts) {
        OneHopExpansionResult result;
        result.sources = source_ids;
        result.edges.reserve(h_sampled_srcs.size());

        for (std::size_t i = 0; i < h_sampled_srcs.size(); ++i) {
            const GlobalId& external_source = projection.mapper().global_id(h_sampled_srcs[i]);

            const GlobalId& external_destination = projection.mapper().global_id(h_sampled_dsts[i]);

            result.edges.emplace_back(external_source, external_destination);
        }

        return result;
    }
};

} // namespace cpgdb

#endif // CPGDB_CUGRAPH_ONE_HOP_EXPAND_EXECUTOR_HPP
