/**
 * @file include/data_model/NeighborRef.hpp
 * @brief Declarations for neighbor ref in the graph data-model module.
 */

//
// Created by akos on 1. 5. 2026.
//

#ifndef CPGDB_NEIGHBORREF_HPP
#define CPGDB_NEIGHBORREF_HPP
#include "EdgeRef.hpp"
#include "NodeRef.hpp"

#include <optional>

namespace cpgdb {
struct NeighborRef {
    NodeRef vertex;
    std::optional<EdgeRef> via_edge;
};

} // namespace cpgdb

#endif // CPGDB_NEIGHBORREF_HPP
