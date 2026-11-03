/**
 * @file include/insertion/ReplicateEverywhereInsertionPlacementPolicy.hpp
 * @brief Full-replication placement with independently selected logical ownership.
 */

#ifndef POLYGRAPH_INSERTION_REPLICATE_EVERYWHERE_INSERTION_PLACEMENT_POLICY_HPP
#define POLYGRAPH_INSERTION_REPLICATE_EVERYWHERE_INSERTION_PLACEMENT_POLICY_HPP

#include "insertion/HomeBackendSelector.hpp"

#include <memory>
#include <vector>

namespace polygraph::insertion {

    /**
     * @brief Writes every entity to all candidate stores and encodes one selected home.
     *
     * When no explicit replica list is supplied, all stores currently registered in
     * the placement context are candidates.
     */
    class ReplicateEverywhereInsertionPlacementPolicy final : public IInsertionPlacementPolicy {
    public:
        /** @brief Creates the policy with a required ownership selector and optional targets. */
        explicit ReplicateEverywhereInsertionPlacementPolicy(std::shared_ptr<const IHomeBackendSelector> home_selector,
                                                             std::vector<BackendId> replica_backends = {});

        [[nodiscard]] PlacementDecision place_node(const Node &node, PlacementContext &context) const override;
        [[nodiscard]] PlacementDecision place_edge(const Edge &edge, PlacementContext &context) const override;

    private:
        /** @brief Returns explicit targets or all registered stores in deterministic order. */
        [[nodiscard]] std::vector<BackendId> candidates(const PlacementContext &context) const;

        std::shared_ptr<const IHomeBackendSelector> home_selector_;
        std::vector<BackendId> replica_backends_;
    };

} // namespace polygraph::insertion

#endif // POLYGRAPH_INSERTION_REPLICATE_EVERYWHERE_INSERTION_PLACEMENT_POLICY_HPP
