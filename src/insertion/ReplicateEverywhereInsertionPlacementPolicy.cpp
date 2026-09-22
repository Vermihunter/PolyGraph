/**
 * @file src/insertion/ReplicateEverywhereInsertionPlacementPolicy.cpp
 * @brief Implementation for replicate everywhere insertion placement policy in the insertion
 * planning module.
 */

#include "insertion/ReplicateEverywhereInsertionPlacementPolicy.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace polygraph::insertion {

    ReplicateEverywhereInsertionPlacementPolicy::ReplicateEverywhereInsertionPlacementPolicy(
            std::shared_ptr<const IHomeBackendSelector> home_selector, std::vector<BackendId> replica_backends) :
        home_selector_(std::move(home_selector)), replica_backends_(std::move(replica_backends)) {
        if (!home_selector_) {
            throw std::invalid_argument("Replicated insertion policy requires a home selector");
        }
    }

    PlacementDecision ReplicateEverywhereInsertionPlacementPolicy::place_node(const Node &node,
                                                                              PlacementContext &context) const {
        auto targets = candidates(context);
        const BackendId home = home_selector_->choose_for_node(node, targets, context);
        return PlacementDecision{
                .target_backends = std::move(targets), .home_backend = home, .encode_home_backend_in_uuid = true};
    }

    PlacementDecision ReplicateEverywhereInsertionPlacementPolicy::place_edge(const Edge &edge,
                                                                              PlacementContext &context) const {
        auto targets = candidates(context);
        const BackendId home = home_selector_->choose_for_edge(edge, targets, context);
        return PlacementDecision{
                .target_backends = std::move(targets), .home_backend = home, .encode_home_backend_in_uuid = true};
    }

    std::vector<BackendId>
    ReplicateEverywhereInsertionPlacementPolicy::candidates(const PlacementContext &context) const {
        std::vector<BackendId> result = replica_backends_;
        if (result.empty()) {
            result.reserve(context.backend_registry.stores().size());
            for (const auto &[backend_id, store]: context.backend_registry.stores()) {
                (void) store;
                result.push_back(backend_id);
            }
        }
        std::ranges::sort(result);
        result.erase(std::unique(result.begin(), result.end()), result.end());
        if (result.empty()) {
            throw std::runtime_error("Replicated insertion policy has no backend candidates");
        }
        return result;
    }

} // namespace polygraph::insertion
