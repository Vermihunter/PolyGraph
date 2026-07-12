/**
 * @file src/algorithms/ExpandSpec.cpp
 * @brief Implementation for expand spec in the graph algorithm module.
 */

#include "algorithms/ExpandSpec.hpp"

#include <algorithm>

namespace cpgdb {

    bool ExpandSpec::allows_edge_type(TypeId edge_type) const noexcept {
        return allowed_edge_types.empty() ||
               std::ranges::find(allowed_edge_types, edge_type) != allowed_edge_types.end();
    }

    bool ExpandSpec::allows_target_type(TypeId node_type) const noexcept {
        return allowed_target_node_types.empty() ||
               std::ranges::find(allowed_target_node_types, node_type) != allowed_target_node_types.end();
    }

} // namespace cpgdb
