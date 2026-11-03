/**
 * @file include/insertion/TypeBasedInsertionPlacementPolicy.hpp
 * @brief Deterministic insertion routing by logical node and edge type.
 */

#ifndef POLYGRAPH_INSERTION_TYPE_BASED_INSERTION_PLACEMENT_POLICY_HPP
#define POLYGRAPH_INSERTION_TYPE_BASED_INSERTION_PLACEMENT_POLICY_HPP

#include "insertion/InsertionPlacementPolicy.hpp"

#include <unordered_map>
#include <vector>

namespace polygraph::insertion {

    /**
     * @brief Routes node and edge types to independently configured backend lists.
     *
     * The first backend in a non-empty route becomes the logical home. Remaining
     * backends are replicas. Optional fallback routes cover unmapped types.
     */
    class TypeBasedInsertionPlacementPolicy final : public IInsertionPlacementPolicy {
    public:
        /// Mapping from logical TypeId to ordered target backends.
        using RouteMap = std::unordered_map<TypeId, std::vector<BackendId>>;

        /**
         * @brief Creates a type router with independent node/edge maps and fallbacks.
         * @param encode_home_backend_in_uuid Encode the first route target into IDs.
         */
        TypeBasedInsertionPlacementPolicy(RouteMap node_routes, RouteMap edge_routes,
                                          std::vector<BackendId> default_node_backends = {},
                                          std::vector<BackendId> default_edge_backends = {},
                                          bool encode_home_backend_in_uuid = false);

        [[nodiscard]] PlacementDecision place_node(const Node &node, PlacementContext &context) const override;
        [[nodiscard]] PlacementDecision place_edge(const Edge &edge, PlacementContext &context) const override;

    private:
        /** @brief Resolves @p type through @p routes and then @p fallback. */
        [[nodiscard]] PlacementDecision decide(TypeId type, const RouteMap &routes,
                                               const std::vector<BackendId> &fallback) const;

        RouteMap node_routes_;
        RouteMap edge_routes_;
        std::vector<BackendId> default_node_backends_;
        std::vector<BackendId> default_edge_backends_;
        bool encode_home_backend_in_uuid_ = false;
    };

} // namespace polygraph::insertion

#endif // POLYGRAPH_INSERTION_TYPE_BASED_INSERTION_PLACEMENT_POLICY_HPP
