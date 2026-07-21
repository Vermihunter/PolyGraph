/**
 * @file src/algorithms/bfs/BfsExecutor.cpp
 * @brief Implementation for bfs executor in the graph algorithm module.
 */

#include "algorithms/bfs/BfsExecutor.hpp"
#include "utilities/uuid.hpp"

#include "logging/level/Debug.hpp"
#include "utilities/Profiling.hpp"
#include "utilities/logging.hpp"

#include <future>
#include <iterator>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "logging/level/Info.hpp"

namespace cpgdb {
    namespace {

        constexpr std::size_t expected_vertex_count = 65'536;

    } // namespace

    BfsExecutor::BfsExecutor(const PlacementCatalog &catalog) noexcept : catalog_(catalog), expander_(catalog) {}

    void BfsExecutor::DenseBfsState::reserve(std::size_t size) {
        to_dense.reserve(size);
        to_global.reserve(size);
        distance.reserve(size);
        predecessor.reserve(size);
    }

    std::uint32_t BfsExecutor::DenseBfsState::get_or_add(const GlobalId &id) {
        if (const auto iterator = to_dense.find(id); iterator != to_dense.end()) {
            return iterator->second;
        }

        if (to_global.size() >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::overflow_error("BFS dense vertex index exceeds uint32_t capacity");
        }

        const auto dense_id = static_cast<std::uint32_t>(to_global.size());
        to_dense.emplace(id, dense_id);
        to_global.push_back(id);
        distance.push_back(-1);
        predecessor.push_back(no_predecessor);
        return dense_id;
    }

    BfsResult BfsExecutor::run(const NodeRef &source, const ExpandSpec &spec, const BfsOptions &options) const {
        if (options.allow_native_pushdown) {
            if (const auto backend = catalog_.choose_native_bfs_backend(source, spec, options)) {
                return catalog_.store(*backend)->run_bfs(source, spec, options);
            }
        }
        return run_distributed_frontier_bfs(source, spec, options);
    }

    BfsResult BfsExecutor::run_streaming(const NodeRef &source, const ExpandSpec &spec, const BfsOptions &options,
                                         const std::function<void(const BfsLayerEvent &)> &on_layer) const {
        if (!on_layer) {
            throw std::invalid_argument("BFS streaming callback must not be empty");
        }
        return run_distributed_frontier_bfs_streaming(source, spec, options, on_layer);
    }

    BfsResult BfsExecutor::run_distributed_frontier_bfs(const NodeRef &source, const ExpandSpec &spec,
                                                        const BfsOptions &options) const {
        EVENT_SCOPE_INFO(full_bfs, "full_bfs", {"source-uuid", global_id_to_string(source.global_id)});
        DenseBfsState state;
        state.reserve(expected_vertex_count);

        const std::uint32_t source_dense = state.get_or_add(source.global_id);
        state.distance[source_dense] = 0;

        std::vector<NodeRef> frontier{source};
        std::uint32_t depth = 0;

        while (!frontier.empty() && (!options.max_depth || depth < *options.max_depth)) {
            std::vector<MinimalAdjacencyRow> rows;
            {
                EVENT_SCOPE_INFO(expansion_layer, "bfs.expand_layer");
                rows = expander_.expand_layer(frontier, spec, static_cast<int>(depth), options);
            }


            std::vector<NodeRef> next_frontier;
            next_frontier.reserve(std::accumulate(rows.begin(), rows.end(), std::size_t{0},
                                                  [](const std::size_t total, const MinimalAdjacencyRow &row) {
                                                      return total + row.neighbors.size();
                                                  }));

            {
                EVENT_SCOPE_INFO(layer_span, "bfs.row_processing", {"depth", depth}, {"row_size", rows.size()});

                for (const MinimalAdjacencyRow &row: rows) {
                    const std::uint32_t row_source = state.get_or_add(row.source.global_id);
                    for (const NodeRef &neighbor: row.neighbors) {
                        if (!spec.allows_target_type(neighbor.type)) {
                            continue;
                        }

                        const std::uint32_t neighbor_dense = state.get_or_add(neighbor.global_id);
                        if (state.distance[neighbor_dense] != -1) {
                            continue;
                        }

                        state.distance[neighbor_dense] = static_cast<int>(depth + 1);
                        if (options.return_predecessors) {
                            state.predecessor[neighbor_dense] = row_source;
                        }
                        next_frontier.push_back(neighbor);
                    }
                }
            }

            frontier = std::move(next_frontier);
            ++depth;
        }

        return make_result(state, options.return_predecessors);
    }

    BfsResult BfsExecutor::run_distributed_frontier_bfs_streaming(
            const NodeRef &source, const ExpandSpec &spec, const BfsOptions &options,
            const std::function<void(const BfsLayerEvent &)> &on_layer) const {
        CPGDB_PROFILE_FUNCTION();

        DenseBfsState state;
        state.reserve(expected_vertex_count);

        const std::uint32_t source_dense = state.get_or_add(source.global_id);
        state.distance[source_dense] = 0;

        std::vector<NodeRef> frontier{source};
        on_layer(BfsLayerEvent{.depth = 0, .vertices = {source}, .tree_edges = {}});

        std::uint32_t depth = 0;
        while (!frontier.empty() && (!options.max_depth || depth < *options.max_depth)) {
            auto rows = expander_.expand_layer(frontier, spec, static_cast<int>(depth), options);

            const auto candidate_count = std::accumulate(rows.begin(), rows.end(), std::size_t{0},
                                                         [](const std::size_t total, const MinimalAdjacencyRow &row) {
                                                             return total + row.neighbors.size();
                                                         });

            std::vector<NodeRef> next_frontier;
            next_frontier.reserve(candidate_count);
            BfsLayerEvent layer{.depth = depth + 1, .vertices = {}, .tree_edges = {}};
            layer.vertices.reserve(candidate_count);
            layer.tree_edges.reserve(candidate_count);

            for (const MinimalAdjacencyRow &row: rows) {
                const std::uint32_t row_source = state.get_or_add(row.source.global_id);
                for (const NodeRef &neighbor: row.neighbors) {
                    if (!spec.allows_target_type(neighbor.type)) {
                        continue;
                    }

                    const std::uint32_t neighbor_dense = state.get_or_add(neighbor.global_id);
                    if (state.distance[neighbor_dense] != -1) {
                        continue;
                    }

                    state.distance[neighbor_dense] = static_cast<int>(depth + 1);
                    if (options.return_predecessors) {
                        state.predecessor[neighbor_dense] = row_source;
                    }

                    next_frontier.push_back(neighbor);
                    layer.vertices.push_back(neighbor);
                    layer.tree_edges.push_back(BfsTreeEdgeRef{.source = row.source, .target = neighbor});
                }
            }

            if (!layer.vertices.empty()) {
                on_layer(layer);
            }

            frontier = std::move(next_frontier);
            ++depth;
        }

        return make_result(state, options.return_predecessors);
    }

    BfsResult BfsExecutor::make_result(const DenseBfsState &state, bool include_predecessors) {
        BfsResult result;
        result.distance.reserve(state.to_global.size());
        if (include_predecessors) {
            result.predecessor.reserve(state.to_global.size());
        }

        for (std::size_t dense = 0; dense < state.to_global.size(); ++dense) {
            if (state.distance[dense] < 0) {
                continue;
            }

            result.distance.emplace(state.to_global[dense], static_cast<std::uint32_t>(state.distance[dense]));

            if (include_predecessors && state.predecessor[dense] != DenseBfsState::no_predecessor) {
                result.predecessor.emplace(state.to_global[dense], state.to_global[state.predecessor[dense]]);
            }
        }

        return result;
    }

} // namespace cpgdb
