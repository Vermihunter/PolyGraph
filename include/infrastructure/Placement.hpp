//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_PLACEMENT_HPP
#define CPGDB_PLACEMENT_HPP

#include <unordered_set>

#include "BackendId.hpp"
#include "BackendKind.hpp"
#include "PlacementKind.hpp"

/**
 * Mapping from logical graph pieces to physical backends
 * Example: Person/FOLLOWS edges are in Neo4j and mirrored in Memgraph
 */
struct Placement {
    BackendId backend_id;
    PlacementKind kind;

    // Which source node types can this placement expand from?
    std::unordered_set<TypeId> source_node_types;

    // Which edge types are physically available here?
    std::unordered_set<TypeId> edge_types;


    std::unordered_set<TypeId> target_node_types;

    // True if this placement owns the authoritative copy of these edges.
    // False for cache/projection/mirror.
    bool authoritative = false;

    // Smaller value = preferred for partial expansion.
    int priority = 100;

    [[nodiscard]] bool can_expand_from(const NodeRef& source,
                                       const ExpandSpec& spec) const {
        /*
         * A placement describes the physical place where a logical edge type
         * can be expanded. For directed traversal, expansion is allowed only
         * from the declared source side. For undirected/BOTH traversal, the
         * frontier vertex may also be on the declared target side.
         */
        const bool matches_forward_side =
            source_node_types.empty() ||
            source_node_types.contains(source.type);

        const bool matches_reverse_side =
            spec.direction != Direction::Directed &&
            !target_node_types.empty() &&
            target_node_types.contains(source.type);

        if (!matches_forward_side && !matches_reverse_side) {
            return false;
        }

        if (spec.allowed_edge_types.empty()) {
            return authoritative || kind == PlacementKind::Cache ||
                   kind == PlacementKind::GpuProjection;
        }

        for (TypeId edge_type : spec.allowed_edge_types) {
            if (edge_types.contains(edge_type)) {
                return true;
            }
        }

        return false;
    }
};

#endif //CPGDB_PLACEMENT_HPP