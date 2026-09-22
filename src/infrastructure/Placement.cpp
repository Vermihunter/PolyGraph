/**
 * @file src/infrastructure/Placement.cpp
 * @brief Implementation for placement in the backend infrastructure module.
 */

#include "infrastructure/Placement.hpp"

#include <algorithm>

namespace polygraph {

    bool Placement::can_expand_from(const NodeRef &source, const ExpandSpec &spec) const {
        const bool matches_forward_side = source_node_types.empty() || source_node_types.contains(source.type);
        const bool matches_reverse_side = spec.direction != Direction::Directed && !target_node_types.empty() &&
                                          target_node_types.contains(source.type);

        if (!matches_forward_side && !matches_reverse_side) {
            return false;
        }

        if (spec.allowed_edge_types.empty()) {
            return authoritative || kind == PlacementKind::Cache || kind == PlacementKind::GpuProjection;
        }

        return std::ranges::any_of(spec.allowed_edge_types,
                                   [&](const TypeId edge_type) { return edge_types.contains(edge_type); });
        /*
        for (const TypeId edge_type : spec.allowed_edge_types) {
            if (edge_types.contains(edge_type)) {
                return true;
            }
        }
        return false;
        */
    }

} // namespace polygraph
