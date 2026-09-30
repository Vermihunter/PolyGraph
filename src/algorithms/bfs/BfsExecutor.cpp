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

#include <algorithm>
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
        if (effective_options.execution_policy == BfsExecutionPolicy::DirectionOptimized) {
            return run_direction_optimized_bfs(source, spec, effective_options, {});
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
        if (effective_options.execution_policy == BfsExecutionPolicy::DirectionOptimized) {
            return run_direction_optimized_bfs(source, spec, effective_options, on_layer);
        }
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

                EVENT_SCOPE_INFO_FIELD(expansion_layer, "candidate_count",
                                       candidate_count.load(std::memory_order_relaxed));
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "candidate_batch_count",
                                       emitted_batch_count.load(std::memory_order_relaxed));
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "new_vertex_count", next_frontier.size());
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "visited_vertex_count", result.distance.size());
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

    BfsResult
    BfsExecutor::run_direction_optimized_bfs(const NodeRef &source, const ExpandSpec &spec, const BfsOptions &options,
                                             const std::function<void(const BfsLayerEvent &)> &on_layer) const {
        if (options.pull_enter_frontier_percent == 0 || options.pull_enter_frontier_percent > 100 ||
            options.pull_exit_frontier_percent > 100 ||
            options.pull_exit_frontier_percent > options.pull_enter_frontier_percent) {
            throw std::invalid_argument(
                    "Direction-optimized BFS requires 0 <= pull-exit <= pull-enter <= 100 and pull-enter > 0");
        }
        if (options.bitmap_frontier_min_size == 0 || options.bitmap_frontier_density_percent == 0 ||
            options.bitmap_frontier_density_percent > 100) {
            throw std::invalid_argument(
                    "Direction-optimized BFS requires a positive bitmap minimum and density percentage in 1..100");
        }

        // Direction optimization is a best-of-both policy: if no exact complete
        // pull backend is available, use the already tuned classic executor with
        // no dense-state or frontier-representation overhead.
        const auto pull_backend = catalog_.choose_pull_backend(spec, options);
        if (!pull_backend) {
            EVENT_SCOPE_INFO(fallback_span, "full_bfs.direction_optimized_fallback",
                             {"source-uuid", global_id_to_string(source.global_id)},
                             {"reason", "no-complete-pull-backend"},
                             {"return_predecessors", options.return_predecessors});
            if (!options.return_predecessors) {
                return run_distributed_frontier_bfs_candidates(source, spec, options, on_layer);
            }
            if (on_layer) {
                return run_distributed_frontier_bfs_streaming(source, spec, options, on_layer);
            }
            return run_distributed_frontier_bfs(source, spec, options);
        }

        const auto pull_store = catalog_.store(*pull_backend);
        const std::size_t pull_universe = pull_store ? pull_store->pull_vertex_count() : 0U;
        if (!pull_store || pull_universe == 0U) {
            EVENT_SCOPE_INFO(fallback_span, "full_bfs.direction_optimized_fallback",
                             {"source-uuid", global_id_to_string(source.global_id)},
                             {"reason", "invalid-complete-pull-backend"},
                             {"return_predecessors", options.return_predecessors});
            if (!options.return_predecessors) {
                return run_distributed_frontier_bfs_candidates(source, spec, options, on_layer);
            }
            if (on_layer) {
                return run_distributed_frontier_bfs_streaming(source, spec, options, on_layer);
            }
            return run_distributed_frontier_bfs(source, spec, options);
        }

        EVENT_SCOPE_INFO(full_bfs, "full_bfs.direction_optimized",
                         {"source-uuid", global_id_to_string(source.global_id)},
                         {"execution_policy", std::string(to_string(options.execution_policy))},
                         {"pull_backend_id", *pull_backend}, {"pull_vertex_count", pull_universe},
                         {"pull_enter_percent", options.pull_enter_frontier_percent},
                         {"pull_exit_percent", options.pull_exit_frontier_percent},
                         {"bitmap_min_size", options.bitmap_frontier_min_size},
                         {"bitmap_density_percent", options.bitmap_frontier_density_percent});

        DenseBfsState state;
        state.reserve(expected_vertex_count);
        const std::uint32_t source_dense = state.get_or_add(source.global_id);
        state.distance[source_dense] = 0;

        std::vector<NodeRef> frontier{source};
        if (on_layer) {
            on_layer(BfsLayerEvent{.depth = 0, .vertices = {source}, .tree_edges = {}});
        }

        bool pull_mode = false;
        std::uint32_t depth = 0;

        while (!frontier.empty() && (!options.max_depth || depth < *options.max_depth)) {
            const double pull_density =
                    100.0 * static_cast<double>(frontier.size()) / static_cast<double>(pull_universe);
            if (!pull_mode && pull_density >= static_cast<double>(options.pull_enter_frontier_percent)) {
                pull_mode = true;
            } else if (pull_mode && pull_density <= static_cast<double>(options.pull_exit_frontier_percent)) {
                pull_mode = false;
            }

            // Frontier membership is only used by bottom-up pull.  Do not build
            // a sorted sparse vector or bitmap on push levels.
            const std::size_t known_vertices = std::max<std::size_t>(state.to_global.size(), 1U);
            const double known_density =
                    100.0 * static_cast<double>(frontier.size()) / static_cast<double>(known_vertices);
            const bool use_bitmap = pull_mode && frontier.size() >= options.bitmap_frontier_min_size &&
                                    known_density >= static_cast<double>(options.bitmap_frontier_density_percent);

            EVENT_SCOPE_INFO(
                    direction_span, "bfs.direction_decision", {"depth", depth}, {"frontier_size", frontier.size()},
                    {"visited_vertex_count", state.to_global.size()}, {"direction", pull_mode ? "pull" : "push"},
                    {"frontier_representation", pull_mode ? (use_bitmap ? "bitmap" : "sparse") : "none"},
                    {"frontier_known_density_percent", known_density}, {"pull_frontier_density_percent", pull_density},
                    {"pull_capable", true}, {"pull_backend_id", *pull_backend}, {"pull_vertex_count", pull_universe});

            std::vector<NodeRef> next_frontier;
            BfsLayerEvent layer{.depth = depth + 1, .vertices = {}, .tree_edges = {}};
            std::size_t candidate_count = 0;

            if (pull_mode) {
                std::vector<GlobalId> sparse_frontier;
                std::vector<std::uint64_t> frontier_bitmap;
                if (use_bitmap) {
                    frontier_bitmap.assign((state.to_global.size() + 63U) / 64U, 0U);
                    for (const NodeRef &vertex: frontier) {
                        const auto dense = state.to_dense.find(vertex.global_id);
                        if (dense == state.to_dense.end()) {
                            throw std::logic_error("BFS frontier contains a vertex missing from dense state");
                        }
                        frontier_bitmap[dense->second / 64U] |= (std::uint64_t{1} << (dense->second % 64U));
                    }
                } else {
                    sparse_frontier.reserve(frontier.size());
                    for (const NodeRef &vertex: frontier) {
                        sparse_frontier.push_back(vertex.global_id);
                    }
                    std::ranges::sort(sparse_frontier);
                    sparse_frontier.erase(std::unique(sparse_frontier.begin(), sparse_frontier.end()),
                                          sparse_frontier.end());
                }

                const auto frontier_contains = [&](const GlobalId id) {
                    if (!use_bitmap) {
                        return std::ranges::binary_search(sparse_frontier, id);
                    }
                    const auto dense = state.to_dense.find(id);
                    if (dense == state.to_dense.end()) {
                        return false;
                    }
                    const std::size_t word = dense->second / 64U;
                    if (word >= frontier_bitmap.size()) {
                        return false;
                    }
                    return (frontier_bitmap[word] & (std::uint64_t{1} << (dense->second % 64U))) != 0U;
                };

                const auto visited_contains = [&](const GlobalId id) {
                    const auto dense = state.to_dense.find(id);
                    return dense != state.to_dense.end() && state.distance[dense->second] >= 0;
                };

                EVENT_SCOPE_INFO(expansion_layer, "bfs.expand_layer.pull", {"depth", depth},
                                 {"frontier_size", frontier.size()}, {"pull_vertex_count", pull_universe},
                                 {"frontier_representation", use_bitmap ? "bitmap" : "sparse"});
                pull_store->stream_pull_candidates(
                        spec, static_cast<int>(depth), options, frontier_contains, visited_contains,
                        [&](const std::span<const PullCandidate> candidates) {
                            candidate_count += candidates.size();
                            for (const PullCandidate &candidate: candidates) {
                                if (!spec.allows_target_type(candidate.vertex.type)) {
                                    continue;
                                }
                                const std::uint32_t target_dense = state.get_or_add(candidate.vertex.global_id);
                                if (state.distance[target_dense] != -1) {
                                    continue;
                                }
                                const auto predecessor_dense = state.to_dense.find(candidate.predecessor.global_id);
                                if (predecessor_dense == state.to_dense.end() ||
                                    state.distance[predecessor_dense->second] != static_cast<int>(depth) ||
                                    !frontier_contains(candidate.predecessor.global_id)) {
                                    throw std::logic_error(
                                            "Pull backend emitted a predecessor outside the current frontier");
                                }

                                state.distance[target_dense] = static_cast<int>(depth + 1);
                                if (options.return_predecessors) {
                                    state.predecessor[target_dense] = predecessor_dense->second;
                                }
                                next_frontier.push_back(candidate.vertex);
                                if (on_layer) {
                                    layer.vertices.push_back(candidate.vertex);
                                    if (options.return_predecessors) {
                                        layer.tree_edges.push_back(BfsTreeEdgeRef{.source = candidate.predecessor,
                                                                                  .target = candidate.vertex});
                                    }
                                }
                            }
                        });
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "candidate_count", candidate_count);
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "new_vertex_count", next_frontier.size());
            } else if (options.return_predecessors) {
                EVENT_SCOPE_INFO(expansion_layer, "bfs.expand_layer.push", {"depth", depth},
                                 {"frontier_size", frontier.size()});
                auto rows = expander_.expand_layer(frontier, spec, static_cast<int>(depth), options);
                candidate_count = std::accumulate(rows.begin(), rows.end(), std::size_t{0},
                                                  [](const std::size_t total, const MinimalAdjacencyRow &row) {
                                                      return total + row.neighbors.size();
                                                  });
                next_frontier.reserve(candidate_count);
                if (on_layer) {
                    layer.vertices.reserve(candidate_count);
                    layer.tree_edges.reserve(candidate_count);
                }

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
                        state.predecessor[neighbor_dense] = row_source;
                        next_frontier.push_back(neighbor);
                        if (on_layer) {
                            layer.vertices.push_back(neighbor);
                            layer.tree_edges.push_back(BfsTreeEdgeRef{.source = row.source, .target = neighbor});
                        }
                    }
                }
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "candidate_count", candidate_count);
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "new_vertex_count", next_frontier.size());
            } else {
                EVENT_SCOPE_INFO(expansion_layer, "bfs.expand_layer.push.candidate_streaming", {"depth", depth},
                                 {"frontier_size", frontier.size()});
                std::mutex discovery_mutex;
                std::atomic_size_t streamed_candidates{0};
                expander_.expand_layer_candidates(
                        frontier, spec, static_cast<int>(depth), options,
                        [&](const std::span<const NodeRef> candidates) {
                            streamed_candidates.fetch_add(candidates.size(), std::memory_order_relaxed);
                            std::scoped_lock lock(discovery_mutex);
                            for (const NodeRef &neighbor: candidates) {
                                if (!spec.allows_target_type(neighbor.type)) {
                                    continue;
                                }
                                const std::uint32_t neighbor_dense = state.get_or_add(neighbor.global_id);
                                if (state.distance[neighbor_dense] != -1) {
                                    continue;
                                }
                                state.distance[neighbor_dense] = static_cast<int>(depth + 1);
                                next_frontier.push_back(neighbor);
                                if (on_layer) {
                                    layer.vertices.push_back(neighbor);
                                }
                            }
                        });
                candidate_count = streamed_candidates.load(std::memory_order_relaxed);
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "candidate_count", candidate_count);
                EVENT_SCOPE_INFO_FIELD(expansion_layer, "new_vertex_count", next_frontier.size());
            }

            EVENT_SCOPE_INFO_FIELD(direction_span, "candidate_count", candidate_count);
            EVENT_SCOPE_INFO_FIELD(direction_span, "next_frontier_size", next_frontier.size());
            EVENT_SCOPE_INFO_FIELD(direction_span, "visited_vertex_count_end", state.to_global.size());

            if (on_layer && !layer.vertices.empty()) {
                on_layer(layer);
            }
            frontier = std::move(next_frontier);
            ++depth;
        }

        EVENT_SCOPE_INFO_FIELD(full_bfs, "result_vertices", state.to_global.size());
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

} // namespace polygraph
