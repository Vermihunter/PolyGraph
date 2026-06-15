//
// Created by akos on 1. 5. 2026.
//

#ifndef CPGDB_BFSEXECUTOR_HPP
#define CPGDB_BFSEXECUTOR_HPP
#include <future>
#include <memory>
#include <unordered_map>

#include "BfsResult.hpp"
#include "algorithms/ExpandSpec.hpp"
#include "data_access/IGraphStore.hpp"
#include "data_model/Node.hpp"
#include "data_model/Value.hpp"
#include "infrastructure/PlacementCatalog.hpp"
#include "utilities/Profiling.hpp"

class BfsExecutor {
public:
    explicit BfsExecutor(const PlacementCatalog& catalog)
        : catalog_(catalog) {}

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
    BfsResult run_distributed_frontier_bfs(const NodeRef& source,
                                           const ExpandSpec& spec,
                                           const BfsOptions& options) const {
        CPGDB_PROFILE_FUNCTION();
        BfsResult result;

        result.distance.emplace(source.global_id, 0);

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
            //auto rows = expand_layer_sequential(layer_plan, spec);

            std::vector<NodeRef> next_frontier;

            for (const auto& row : rows) {
                for (const auto& neighbor : row.neighbors) {

                    if (!spec.allows_target_type(neighbor.vertex.type)) {
                        continue;
                    }

                    auto [it, inserted] =
                        result.distance.emplace(neighbor.vertex.global_id, depth + 1);

                    if (!inserted) {
                        continue;
                    }

                    if (options.return_predecessors) {
                        result.predecessor.emplace(
                            neighbor.vertex.global_id,
                            row.source.global_id
                        );
                    }

                    next_frontier.push_back(neighbor.vertex);
                }
            }

            frontier = std::move(next_frontier);
            ++depth;
        }

        return result;
    }

    std::vector<AdjacencyRow>
    expand_layer_sequential(const PlacementCatalog::LayerExpansionPlan& plan,
                            const ExpandSpec& spec) const {
        std::vector<AdjacencyRow> merged;

        for (const auto& [backend_id, backend_frontier] : plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);

            auto partial = store->get_neighbors_batch(backend_frontier, spec);

            merged.insert(
                merged.end(),
                std::make_move_iterator(partial.begin()),
                std::make_move_iterator(partial.end())
            );
        }

        return merged;
    }

    std::vector<AdjacencyRow>
    expand_layer_parallel(const PlacementCatalog::LayerExpansionPlan& plan,
                          const ExpandSpec& spec) const {
        std::vector<
            std::future<std::vector<AdjacencyRow>>
        > futures;

        futures.reserve(plan.backend_frontiers.size());

        for (const auto& [backend_id, backend_frontier] : plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);

            futures.push_back(std::async(
                std::launch::async,
                [store, backend_frontier, spec]() {
                    return store->get_neighbors_batch(backend_frontier, spec);
                }
            ));
        }

        std::vector<AdjacencyRow> merged;

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

#endif //CPGDB_BFSEXECUTOR_HPP