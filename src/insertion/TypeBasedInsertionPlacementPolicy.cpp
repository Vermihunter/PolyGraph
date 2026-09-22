/**
 * @file src/insertion/TypeBasedInsertionPlacementPolicy.cpp
 * @brief Implementation for type based insertion placement policy in the insertion planning module.
 */

#include "insertion/TypeBasedInsertionPlacementPolicy.hpp"

#include <stdexcept>
#include <utility>

namespace polygraph::insertion {

    TypeBasedInsertionPlacementPolicy::TypeBasedInsertionPlacementPolicy(RouteMap node_routes, RouteMap edge_routes,
                                                                         std::vector<BackendId> default_node_backends,
                                                                         std::vector<BackendId> default_edge_backends,
                                                                         const bool encode_home_backend_in_uuid) :
        node_routes_(std::move(node_routes)), edge_routes_(std::move(edge_routes)),
        default_node_backends_(std::move(default_node_backends)),
        default_edge_backends_(std::move(default_edge_backends)),
        encode_home_backend_in_uuid_(encode_home_backend_in_uuid) {}

    PlacementDecision TypeBasedInsertionPlacementPolicy::place_node(const Node &node, PlacementContext &) const {
        return decide(node.type, node_routes_, default_node_backends_);
    }

    PlacementDecision TypeBasedInsertionPlacementPolicy::place_edge(const Edge &edge, PlacementContext &) const {
        return decide(edge.type, edge_routes_, default_edge_backends_);
    }

    PlacementDecision TypeBasedInsertionPlacementPolicy::decide(const TypeId type, const RouteMap &routes,
                                                                const std::vector<BackendId> &fallback) const {
        const auto iterator = routes.find(type);
        const std::vector<BackendId> &targets = iterator == routes.end() ? fallback : iterator->second;
        if (targets.empty()) {
            throw std::runtime_error("No insertion route configured for entity type " + std::to_string(type));
        }

        return PlacementDecision{.target_backends = targets,
                                 .home_backend = targets.front(),
                                 .encode_home_backend_in_uuid = encode_home_backend_in_uuid_};
    }

} // namespace polygraph::insertion
