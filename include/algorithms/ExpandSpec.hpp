/**
 * @file include/algorithms/ExpandSpec.hpp
 * @brief Defines one-hop graph expansion filtering and direction semantics.
 */

#ifndef CPGDB_ALGORITHMS_EXPAND_SPEC_HPP
#define CPGDB_ALGORITHMS_EXPAND_SPEC_HPP

#include "domain/graph/Direction.hpp"
#include "domain/graph/TypeId.hpp"

#include <vector>

namespace cpgdb {

/**
 * @brief Describes which incident edges and destination vertices may be traversed.
 *
 * Empty type lists are interpreted as wildcards. The same specification is used
 * by backend queries, placement coverage checks, CPU adjacency expansion, and
 * distributed BFS planning.
 */
struct ExpandSpec {
    /// Logical edge types that may be traversed; empty means every edge type.
    std::vector<TypeId> allowed_edge_types;
    /// Logical destination node types that may be reached; empty means every type.
    std::vector<TypeId> allowed_target_node_types;
    /// Direction in which incident edges are interpreted.
    Direction direction = Direction::Both;
    /// Whether rich adjacency results should include the traversed edge reference.
    bool return_edge_refs = false;

    /** @brief Returns whether @p edge_type passes the edge-type allowlist. */
    [[nodiscard]] bool allows_edge_type(TypeId edge_type) const noexcept;

    /** @brief Returns whether @p node_type passes the destination-type allowlist. */
    [[nodiscard]] bool allows_target_type(TypeId node_type) const noexcept;
};

} // namespace cpgdb

#endif // CPGDB_ALGORITHMS_EXPAND_SPEC_HPP
