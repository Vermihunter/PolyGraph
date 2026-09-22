/**
 * @file src/algorithms/bfs/BfsExecutor.cpp
 * @brief Implementation for bfs executor in the graph algorithm module.
 */

#include "algorithms/bfs/BfsExecutor.hpp"
#include "utilities/uuid.hpp"

#include "events/EventContext.hpp"
#include "events/Events.hpp"
#include "infrastructure/scheduling/BackendRequestScheduler.hpp"
#include "utilities/Profiling.hpp"

#include <atomic>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <utility>


namespace polygraph {
    namespace {

        constexpr std::size_t expected_vertex_count = 65'536;

        [[nodiscard]] BfsOptions with_scheduler_query_id(const BfsOptions &options) {
            BfsOptions result = options;
            if (result.scheduler_query_id == 0) {
                result.scheduler_query_id = next_backend_scheduler_query_id();
            }
            return result;
        }

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
        const BfsOptions effective_options = with_scheduler_query_id(options);
        [[maybe_unused]] events::ScopedQueryExecutionContext event_context(effective_options.scheduler_query_id);
        if (effective_options.allow_native_pushdown) {
            if (const auto backend = catalog_.choose_native_bfs_backend(source, spec, effective_options)) {
                return catalog_.store(*backend)->run_bfs(source, spec, effective_options);
            }
        }
        if (!effective_options.return_predecessors) {
            return run_distributed_frontier_bfs_candidates(source, spec, effective_options, {});
        }
        return run_distributed_frontier_bfs(source, spec, effective_options);
    }

    BfsResult BfsExecutor::run_streaming(const NodeRef &source, const ExpandSpec &spec, const BfsOptions &options,
                                         const std::function<void(const BfsLayerEvent &)> &on_layer) const {
        if (!on_layer) {
            throw std::invalid_argument("BFS streaming callback must not be empty");
        }
        const BfsOptions effective_options = with_scheduler_query_id(options);
        [[maybe_unused]] events::ScopedQueryExecutionContext event_context(effective_options.scheduler_query_id);
        if (!effective_options.return_predecessors) {
            return run_distributed_frontier_bfs_candidates(source, spec, effective_options, on_layer);
        }
        return run_distributed_frontier_bfs_streaming(source, spec, effective_options, on_layer);
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
        POLYGRAPH_PROFILE_FUNCTION();

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

    BfsResult BfsExecutor::run_distributed_frontier_bfs_candidates(
            const NodeRef &source, const ExpandSpec &spec, const BfsOptions &options,
            const std::function<void(const BfsLayerEvent &)> &on_layer) const {
        EVENT_SCOPE_INFO(full_bfs, "full_bfs.candidate_streaming",
                         {"source-uuid", global_id_to_string(source.global_id)},
                         {"streaming_candidate_batch_size", options.streaming_candidate_batch_size},
                         {"scheduler_query_id", options.scheduler_query_id},
                         {"scheduler_priority", options.backend_scheduler_priority},
                         {"scheduler_weight", options.backend_scheduler_weight});

        BfsResult result;
        result.distance.reserve(expected_vertex_count);
        result.distance.emplace(source.global_id, 0U);

        std::vector<NodeRef> frontier{source};
        if (on_layer) {
            on_layer(BfsLayerEvent{.depth = 0, .vertices = {source}, .tree_edges = {}});
        }

        std::uint32_t depth = 0;
        while (!frontier.empty() && (!options.max_depth || depth < *options.max_depth)) {
            std::vector<NodeRef> next_frontier;
            BfsLayerEvent layer{.depth = depth + 1, .vertices = {}, .tree_edges = {}};
            std::mutex discovery_mutex;
            std::atomic_size_t candidate_count{0};
            std::atomic_size_t emitted_batch_count{0};

            {
                EVENT_SCOPE_INFO(expansion_layer, "bfs.expand_layer.candidate_streaming", {"depth", depth},
                                 {"frontier_size", frontier.size()});

                expander_.expand_layer_candidates(
                        frontier, spec, static_cast<int>(depth), options,
                        [&](const std::span<const NodeRef> candidates) {
                            candidate_count.fetch_add(candidates.size(), std::memory_order_relaxed);
                            emitted_batch_count.fetch_add(1, std::memory_order_relaxed);

                            // Backend/query workers may call this concurrently.  One
                            // batch-level lock keeps the visited map and next frontier
                            // single-owner without paying a lock for every edge.
                            std::scoped_lock lock(discovery_mutex);
                            for (const NodeRef &neighbor: candidates) {
                                if (!spec.allows_target_type(neighbor.type)) {
                                    continue;
                                }

                                const auto [iterator, inserted] =
                                        result.distance.emplace(neighbor.global_id, depth + 1);
                                (void) iterator;
                                if (!inserted) {
                                    continue;
                                }

                                next_frontier.push_back(neighbor);
                                if (on_layer) {
                                    layer.vertices.push_back(neighbor);
                                }
                            }
                        });

                expansion_layer.add_end_field("candidate_count", candidate_count.load(std::memory_order_relaxed));
                expansion_layer.add_end_field("candidate_batch_count",
                                              emitted_batch_count.load(std::memory_order_relaxed));
                expansion_layer.add_end_field("new_vertex_count", next_frontier.size());
                expansion_layer.add_end_field("visited_vertex_count", result.distance.size());
            }

            if (on_layer && !layer.vertices.empty()) {
                on_layer(layer);
            }

            frontier = std::move(next_frontier);
            ++depth;
        }

        EVENT_SCOPE_INFO_FIELD(full_bfs, "result_vertices", result.distance.size());
        return result;
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

} // namespace polygraph
