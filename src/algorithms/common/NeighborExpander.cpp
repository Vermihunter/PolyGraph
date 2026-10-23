//
// Created by akos on 11. 7. 2026.
//

#include "algorithms/common/NeighborExpander.hpp"
#include "analysis/AlgorithmAnalyzer.hpp"

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
#if POLYGRAPH_ENABLE_ALGORITHM_ANALYZER
            auto &analyzer = algorithm_analysis::AlgorithmAnalyzer::instance();
            const auto owner_of = [this](const GlobalId id) { return catalog_.owner_backend(id); };
            analyzer.record_node_batch(options.scheduler_query_id, "source", backend_id, backend_frontier, owner_of);
            auto rows = catalog_.store(backend_id)->get_minimal_neighbors_batch(backend_frontier, spec, depth, options);
            analyzer.record_destination_rows(options.scheduler_query_id, backend_id, rows, owner_of);
            return rows;
#else
            return catalog_.store(backend_id)->get_minimal_neighbors_batch(backend_frontier, spec, depth, options);
#endif
        }

        std::vector<std::future<std::vector<MinimalAdjacencyRow>>> futures;
        futures.reserve(plan.backend_frontiers.size());

        for (const auto &[backend_id, backend_frontier]: plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);
            const auto *frontier_ptr = &backend_frontier;
#if POLYGRAPH_ENABLE_ALGORITHM_ANALYZER
            algorithm_analysis::AlgorithmAnalyzer::instance().record_node_batch(
                    options.scheduler_query_id, "source", backend_id, backend_frontier,
                    [this](const GlobalId id) { return catalog_.owner_backend(id); });
#endif
            futures.push_back(std::async(std::launch::async, [this, store = std::move(store), frontier_ptr, backend_id,
                                                              spec, depth, options] {
                auto rows = store->get_minimal_neighbors_batch(*frontier_ptr, spec, depth, options);
#if POLYGRAPH_ENABLE_ALGORITHM_ANALYZER
                algorithm_analysis::AlgorithmAnalyzer::instance().record_destination_rows(
                        options.scheduler_query_id, backend_id, rows,
                        [this](const GlobalId id) { return catalog_.owner_backend(id); });
#endif
                return rows;
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
#if POLYGRAPH_ENABLE_ALGORITHM_ANALYZER
            auto &analyzer = algorithm_analysis::AlgorithmAnalyzer::instance();
            const auto owner_of = [this](const GlobalId id) { return catalog_.owner_backend(id); };
            analyzer.record_node_batch(options.scheduler_query_id, "source", backend_id, backend_frontier, owner_of);
            const NeighborBatchConsumer instrumented = [&, backend_id](const std::span<const NodeRef> candidates) {
                analyzer.record_node_batch(options.scheduler_query_id, "destination", backend_id, candidates, owner_of);
                consumer(candidates);
            };
            catalog_.store(backend_id)
                    ->stream_neighbor_candidates(backend_frontier, spec, depth, options, instrumented);
#else
            catalog_.store(backend_id)->stream_neighbor_candidates(backend_frontier, spec, depth, options, consumer);
#endif
            return;
        }

        std::vector<std::future<void>> futures;
        futures.reserve(plan.backend_frontiers.size());
        for (const auto &[backend_id, backend_frontier]: plan.backend_frontiers) {
            auto store = catalog_.store(backend_id);
            const auto *frontier_ptr = &backend_frontier;
#if POLYGRAPH_ENABLE_ALGORITHM_ANALYZER
            algorithm_analysis::AlgorithmAnalyzer::instance().record_node_batch(
                    options.scheduler_query_id, "source", backend_id, backend_frontier,
                    [this](const GlobalId id) { return catalog_.owner_backend(id); });
#endif
            futures.push_back(std::async(std::launch::async, [this, store = std::move(store), frontier_ptr, backend_id,
                                                              spec, depth, options, consumer] {
#if POLYGRAPH_ENABLE_ALGORITHM_ANALYZER
                auto &analyzer = algorithm_analysis::AlgorithmAnalyzer::instance();
                const auto owner_of = [this](const GlobalId id) { return catalog_.owner_backend(id); };
                const NeighborBatchConsumer instrumented = [&, backend_id](const std::span<const NodeRef> candidates) {
                    analyzer.record_node_batch(options.scheduler_query_id, "destination", backend_id, candidates,
                                               owner_of);
                    consumer(candidates);
                };
                store->stream_neighbor_candidates(*frontier_ptr, spec, depth, options, instrumented);
#else
                        store->stream_neighbor_candidates(*frontier_ptr, spec, depth, options, consumer);
#endif
            }));
        }

        for (auto &future: futures) {
            future.get();
        }
    }

} // namespace polygraph
