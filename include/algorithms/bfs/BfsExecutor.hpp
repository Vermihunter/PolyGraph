//
// Created by akos on 1. 5. 2026.
//

#ifndef CPGDB_BFSEXECUTOR_HPP
#define CPGDB_BFSEXECUTOR_HPP

#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <boost/container_hash/hash.hpp>

#include "BfsResult.hpp"
#include "algorithms/bfs/BfsLayer.hpp"
#include "algorithms/ExpandSpec.hpp"
#include "data_access/IGraphStore.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "data_model/Node.hpp"
#include "data_model/Value.hpp"
#include "infrastructure/PlacementCatalog.hpp"
#include "utilities/Profiling.hpp"

class BfsExecutor {
public:
    explicit BfsExecutor(const PlacementCatalog& catalog)
        : catalog_(catalog) {}



    [[nodiscard]] BfsResult run_streaming(const NodeRef& source,
                                          const ExpandSpec& spec,
                                          const BfsOptions& options,
                                          const std::function<void(const BfsLayerEvent&)>& on_layer) const {
        if (options.allow_native_pushdown) {
            /*
             * Native pushdown BFS usually returns the whole result at once.
             * For true layer-by-layer streaming, use the distributed frontier loop.
             * Keep this disabled for streaming unless the backend exposes a native
             * streaming BFS API later.
             */
        }

        return run_distributed_frontier_bfs_streaming(source, spec, options, on_layer);
    }
    [[nodiscard]] BfsResult run(const NodeRef& source,
                                const ExpandSpec& spec,
                                const BfsOptions& options) const {
        if (options.allow_native_pushdown) {
            auto native_backend =
                catalog_.choose_native_bfs_backend(source, spec, options);

            if (native_backend.has_value()) {
                auto store = catalog_.store(*native_backend);
                return store->run_bfs(source, spec, options);
            }
        }

        return run_distributed_frontier_bfs(source, spec, options);
    }

private:
    struct DenseBfsState {
        std::unordered_map<GlobalId, std::uint32_t, boost::hash<GlobalId>> to_dense;
        std::vector<GlobalId> to_global;
        std::vector<int> distance;
        std::vector<std::uint32_t> predecessor;

        static constexpr std::uint32_t no_predecessor = UINT32_MAX;

        std::uint32_t get_or_add(const GlobalId& id) {
            auto it = to_dense.find(id);
            if (it != to_dense.end()) {
                return it->second;
            }

            const auto dense = static_cast<std::uint32_t>(to_global.size());
            to_dense.emplace(id, dense);
            to_global.push_back(id);
            distance.push_back(-1);
            predecessor.push_back(no_predecessor);
            return dense;
        }
    };

    BfsResult run_distributed_frontier_bfs(const NodeRef& source,
                                           const ExpandSpec& spec,
                                           const BfsOptions& options) const {
        CPGDB_PROFILE_FUNCTION();

        DenseBfsState state;
        state.to_dense.reserve(65536);
        state.to_global.reserve(65536);
        state.distance.reserve(65536);
        state.predecessor.reserve(65536);

        const std::uint32_t source_dense = state.get_or_add(source.global_id);
        state.distance[source_dense] = 0;

        std::vector<NodeRef> frontier;
        frontier.push_back(source);

        std::uint32_t depth = 0;

        while (!frontier.empty()) {
            if (options.max_depth.has_value() &&
                depth >= *options.max_depth) {
                break;
            }

            auto layer_plan =
                catalog_.plan_frontier_expansion(frontier, spec, options);

            auto rows = expand_layer_parallel(layer_plan, spec);

            std::vector<NodeRef> next_frontier;

            for (const auto& row : rows) {
                const std::uint32_t source_row_dense =
                    state.get_or_add(row.source.global_id);

                for (const auto& neighbor : row.neighbors) {
                    if (!spec.allows_target_type(neighbor.type)) {
                        continue;
                    }

                    const std::uint32_t neighbor_dense =
                        state.get_or_add(neighbor.global_id);

                    if (state.distance[neighbor_dense] != -1) {
                        continue;
                    }

                    state.distance[neighbor_dense] = static_cast<int>(depth + 1);

                    if (options.return_predecessors) {
                        state.predecessor[neighbor_dense] = source_row_dense;
                    }

                    next_frontier.push_back(neighbor);
                }
            }

            frontier = std::move(next_frontier);
            ++depth;
        }

        BfsResult result;
        result.distance.reserve(state.to_global.size());

        for (std::uint32_t dense = 0;
             dense < static_cast<std::uint32_t>(state.to_global.size());
             ++dense) {
            if (state.distance[dense] < 0) {
                continue;
            }

            result.distance.emplace(
                state.to_global[dense],
                static_cast<std::uint32_t>(state.distance[dense])
            );

            if (options.return_predecessors &&
                state.predecessor[dense] != DenseBfsState::no_predecessor) {
                result.predecessor.emplace(
                    state.to_global[dense],
                    state.to_global[state.predecessor[dense]]
                );
            }
        }

        return result;
    }



    BfsResult run_distributed_frontier_bfs_streaming(
        const NodeRef& source,
        const ExpandSpec& spec,
        const BfsOptions& options,
        const std::function<void(const BfsLayerEvent&)>& on_layer
    ) const {
        CPGDB_PROFILE_FUNCTION();

        DenseBfsState state;
        state.to_dense.reserve(65536);
        state.to_global.reserve(65536);
        state.distance.reserve(65536);
        state.predecessor.reserve(65536);

        const std::uint32_t source_dense = state.get_or_add(source.global_id);
        state.distance[source_dense] = 0;

        std::vector<NodeRef> frontier;
        frontier.push_back(source);

        BfsLayerEvent source_layer;
        source_layer.depth = 0;
        source_layer.vertices.push_back(source);
        on_layer(source_layer);

        std::uint32_t depth = 0;

        while (!frontier.empty()) {
            if (options.max_depth.has_value() &&
                depth >= *options.max_depth) {
                break;
            }

            auto layer_plan =
                catalog_.plan_frontier_expansion(frontier, spec, options);

            auto rows = expand_layer_parallel(layer_plan, spec);

            std::vector<NodeRef> next_frontier;
            BfsLayerEvent layer;
            layer.depth = depth + 1;

            for (const auto& row : rows) {
                const std::uint32_t source_row_dense =
                    state.get_or_add(row.source.global_id);

                for (const auto& neighbor : row.neighbors) {
                    if (!spec.allows_target_type(neighbor.type)) {
                        continue;
                    }

                    const std::uint32_t neighbor_dense =
                        state.get_or_add(neighbor.global_id);

                    if (state.distance[neighbor_dense] != -1) {
                        continue;
                    }

                    state.distance[neighbor_dense] = static_cast<int>(depth + 1);

                    if (options.return_predecessors) {
                        state.predecessor[neighbor_dense] = source_row_dense;
                    }

                    next_frontier.push_back(neighbor);
                    layer.vertices.push_back(neighbor);
                    layer.tree_edges.push_back(BfsTreeEdgeRef{
                        .source = row.source,
                        .target = neighbor
                    });
                }
            }

            if (!layer.vertices.empty()) {
                on_layer(layer);
            }

            frontier = std::move(next_frontier);
            ++depth;
        }

        BfsResult result;
        result.distance.reserve(state.to_global.size());

        for (std::uint32_t dense = 0;
             dense < static_cast<std::uint32_t>(state.to_global.size());
             ++dense) {
            if (state.distance[dense] < 0) {
                continue;
            }

            result.distance.emplace(
                state.to_global[dense],
                static_cast<std::uint32_t>(state.distance[dense])
            );

            if (options.return_predecessors &&
                state.predecessor[dense] != DenseBfsState::no_predecessor) {
                result.predecessor.emplace(
                    state.to_global[dense],
                    state.to_global[state.predecessor[dense]]
                );
            }
        }

        return result;
    }

    std::vector<MinimalAdjacencyRow>
    expand_layer_sequential(const PlacementCatalog::LayerExpansionPlan& plan,
                            const ExpandSpec& spec) const {
        std::vector<MinimalAdjacencyRow> merged;

        for (const auto& [backend_id, backend_frontier] : plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);
            auto partial = store->get_minimal_neighbors_batch(backend_frontier, spec);

            merged.insert(
                merged.end(),
                std::make_move_iterator(partial.begin()),
                std::make_move_iterator(partial.end())
            );
        }

        return merged;
    }

    std::vector<MinimalAdjacencyRow>
    expand_layer_parallel(const PlacementCatalog::LayerExpansionPlan& plan,
                          const ExpandSpec& spec) const {
        if (plan.backend_frontiers.empty()) {
            return {};
        }

        if (plan.backend_frontiers.size() == 1) {
            const auto& [backend_id, backend_frontier] = *plan.backend_frontiers.begin();
            auto store = catalog_.store(backend_id);
            return store->get_minimal_neighbors_batch(backend_frontier, spec);
        }

        std::vector<std::future<std::vector<MinimalAdjacencyRow>>> futures;
        futures.reserve(plan.backend_frontiers.size());

        for (const auto& [backend_id, backend_frontier] : plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);

            futures.push_back(std::async(
                std::launch::async,
                [store, backend_frontier, spec]() {
                    return store->get_minimal_neighbors_batch(backend_frontier, spec);
                }
            ));
        }

        std::vector<MinimalAdjacencyRow> merged;

        for (auto& future : futures) {
            auto partial = future.get();

            merged.insert(
                merged.end(),
                std::make_move_iterator(partial.begin()),
                std::make_move_iterator(partial.end())
            );
        }

        return merged;
    }

private:
    const PlacementCatalog& catalog_;
};

#endif // CPGDB_BFSEXECUTOR_HPP
