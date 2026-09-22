/**
 * @file include/infrastructure/Placement.hpp
 * @brief Declarations for placement in the backend infrastructure module.
 */

#ifndef POLYGRAPH_INFRASTRUCTURE_PLACEMENT_HPP
#define POLYGRAPH_INFRASTRUCTURE_PLACEMENT_HPP

#include "algorithms/ExpandSpec.hpp"
#include "data_model/NodeRef.hpp"
#include "infrastructure/BackendId.hpp"
#include "infrastructure/PlacementKind.hpp"

#include <unordered_set>

namespace polygraph {

struct Placement {
    BackendId backend_id;
    PlacementKind kind;
    std::unordered_set<TypeId> source_node_types;
    std::unordered_set<TypeId> edge_types;
    std::unordered_set<TypeId> target_node_types;
    bool authoritative = false;
    int priority = 100;

    [[nodiscard]] bool can_expand_from(const NodeRef& source, const ExpandSpec& spec) const;
};

} // namespace polygraph

#endif // POLYGRAPH_INFRASTRUCTURE_PLACEMENT_HPP
