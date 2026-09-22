//
// Created by akos on 11. 7. 2026.
//

#include "algorithms/common/NeighborExpander.hpp"

#include <future>

namespace polygraph {
    [[nodiscard]] std::vector<MinimalAdjacencyRow> NeighborExpander::expand_layer(const std::vector<NodeRef> &frontier,
                                                                                  const ExpandSpec &spec, int depth,
                                                                                  const BfsOptions &options) const {
        auto plan = catalog_.plan_frontier_expansion(frontier, spec, options);

        if (!options.allow_partial_acceleration) {
            throw std::runtime_error("Sequential expansion is not supported");
        }
        return expand_layer_parallel(plan, spec, depth, options);
    }

    std::vector<MinimalAdjacencyRow>
    NeighborExpander::expand_layer_parallel(const PlacementCatalog::LayerExpansionPlan &plan, const ExpandSpec &spec,
                                            int depth, const BfsOptions &options) const {

        if (plan.backend_frontiers.empty()) {
            return {};
        }

        if (plan.backend_frontiers.size() == 1) {
            const auto &[backend_id, backend_frontier] = *plan.backend_frontiers.begin();
            return catalog_.store(backend_id)->get_minimal_neighbors_batch(backend_frontier, spec, depth, options);
        }

        std::vector<std::future<std::vector<MinimalAdjacencyRow>>> futures;
        futures.reserve(plan.backend_frontiers.size());

        for (const auto &[backend_id, backend_frontier]: plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);
            const auto *frontier_ptr = &backend_frontier;
            futures.push_back(
                    std::async(std::launch::async, [store = std::move(store), frontier_ptr, spec, depth, options] {
                        return store->get_minimal_neighbors_batch(*frontier_ptr, spec, depth, options);
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

    void NeighborExpander::expand_layer_candidates(const std::vector<NodeRef> &frontier, const ExpandSpec &spec,
                                                   int depth, const BfsOptions &options,
                                                   const NeighborBatchConsumer &consumer) const {
        if (!consumer || frontier.empty()) {
            return;
        }

        auto plan = catalog_.plan_frontier_expansion(frontier, spec, options);
        if (!options.allow_partial_acceleration) {
            throw std::runtime_error("Sequential expansion is not supported");
        }
        if (plan.backend_frontiers.empty()) {
            return;
        }

        if (plan.backend_frontiers.size() == 1) {
            const auto &[backend_id, backend_frontier] = *plan.backend_frontiers.begin();
            catalog_.store(backend_id)->stream_neighbor_candidates(backend_frontier, spec, depth, options, consumer);
            return;
        }

        std::vector<std::future<void>> futures;
        futures.reserve(plan.backend_frontiers.size());
        for (const auto &[backend_id, backend_frontier]: plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);
            const auto *frontier_ptr = &backend_frontier;
            futures.push_back(std::async(
                    std::launch::async, [store = std::move(store), frontier_ptr, spec, depth, options, consumer] {
                        store->stream_neighbor_candidates(*frontier_ptr, spec, depth, options, consumer);
                    }));
        }

        for (auto &future: futures) {
            future.get();
        }
    }

} // namespace polygraph
