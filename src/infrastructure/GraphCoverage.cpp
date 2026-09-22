/**
 * @file src/infrastructure/GraphCoverage.cpp
 * @brief Implementation for graph coverage in the backend infrastructure module.
 */

#include "infrastructure/GraphCoverage.hpp"

#include <algorithm>

namespace polygraph {

    bool GraphCoverage::covers_node_type(TypeId type) const noexcept {
        return covers_all_node_types || node_types.contains(type);
    }

    bool GraphCoverage::covers_edge_type(TypeId type) const noexcept {
        return covers_all_edge_types || edge_types.contains(type);
    }

    bool GraphCoverage::covers_expand_spec(const NodeRef &source, const ExpandSpec &spec) const {
        if (!fresh || !covers_node_type(source.type)) {
            return false;
        }

        const bool covers_edges = spec.allowed_edge_types.empty()
                                          ? covers_all_edge_types
                                          : std::ranges::all_of(spec.allowed_edge_types,
                                                                [this](TypeId type) { return covers_edge_type(type); });
        if (!covers_edges) {
            return false;
        }

        return spec.allowed_target_node_types.empty()
                       ? covers_all_node_types
                       : std::ranges::all_of(spec.allowed_target_node_types,
                                             [this](TypeId type) { return covers_node_type(type); });
    }

    bool GraphCoverage::can_answer_whole_bfs(const NodeRef &source, const ExpandSpec &spec) const {
        return complete_for_requested_subgraph && covers_expand_spec(source, spec);
    }

} // namespace polygraph
