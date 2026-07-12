//
// Created by akos on 11. 7. 2026.
//

#include "algorithms/common/NeighborExpander.hpp"

#include <future>

namespace cpgdb {
    [[nodiscard]] std::vector<MinimalAdjacencyRow> NeighborExpander::expand_layer(const std::vector<NodeRef> &frontier,
                                                                                  const ExpandSpec &spec, int depth,
                                                                                  bool parallel) const {
        auto plan = catalog_.plan_frontier_expansion(frontier, spec, BfsOptions{.max_depth = 1});

        if (!parallel) {
            throw std::runtime_error("Sequential expansion is not supported");
        }
        return expand_layer_parallel(plan, spec, depth);
    }

    std::vector<MinimalAdjacencyRow>
    NeighborExpander::expand_layer_parallel(const PlacementCatalog::LayerExpansionPlan &plan, const ExpandSpec &spec,
                                            int depth) const {

        if (plan.backend_frontiers.empty()) {
            return {};
        }

        if (plan.backend_frontiers.size() == 1) {
            const auto &[backend_id, backend_frontier] = *plan.backend_frontiers.begin();
            return catalog_.store(backend_id)->get_minimal_neighbors_batch(backend_frontier, spec, depth);
        }

        std::vector<std::future<std::vector<MinimalAdjacencyRow>>> futures;
        futures.reserve(plan.backend_frontiers.size());

        for (const auto &[backend_id, backend_frontier]: plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);
            futures.push_back(std::async(std::launch::async, [store = std::move(store), backend_frontier, spec, depth] {
                return store->get_minimal_neighbors_batch(backend_frontier, spec, depth);
            }));
        }

        std::vector<MinimalAdjacencyRow> merged;
        for (auto &future: futures) {
            auto partial = future.get();
            merged.reserve(merged.size() + partial.size());
            std::move(partial.begin(), partial.end(), std::back_inserter(merged));
        }
        return merged;
    }

} // namespace cpgdb
