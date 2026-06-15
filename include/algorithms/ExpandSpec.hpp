//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_EXPANDSPEC_HPP
#define CPGDB_EXPANDSPEC_HPP

#include <vector>
#include <optional>

#include "domain/graph/Direction.hpp"
#include "domain/graph/TypeId.hpp"

struct ExpandSpec {
    std::vector<TypeId> allowed_edge_types;
    std::vector<TypeId> allowed_target_node_types;
    Direction direction = Direction::Both;

    // Fast algorithms such as BFS only need neighbor vertices.
    // Set this to true only when the caller really needs EdgeRef metadata.
    bool return_edge_refs = false;

    [[nodiscard]] bool allows_edge_type(TypeId edge_type) const {
        return allowed_edge_types.empty() ||
               std::find(
                   allowed_edge_types.begin(),
                   allowed_edge_types.end(),
                   edge_type
               ) != allowed_edge_types.end();
    }

    [[nodiscard]] bool allows_target_type(TypeId node_type) const {
        return allowed_target_node_types.empty() ||
               std::find(
                   allowed_target_node_types.begin(),
                   allowed_target_node_types.end(),
                   node_type
               ) != allowed_target_node_types.end();
    }
};


#endif //CPGDB_EXPANDSPEC_HPP