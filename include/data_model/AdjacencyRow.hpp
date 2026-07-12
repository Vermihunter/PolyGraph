/**
 * @file include/data_model/AdjacencyRow.hpp
 * @brief Declarations for adjacency row in the graph data-model module.
 */

//
// Created by akos on 1. 5. 2026.
//

#ifndef CPGDB_ADJACENCYROW_HPP
#define CPGDB_ADJACENCYROW_HPP
#include "NeighborRef.hpp"
#include "Node.hpp"

namespace cpgdb {
struct AdjacencyRow {
    NodeRef source;
    std::vector<NeighborRef> neighbors;
};

} // namespace cpgdb

#endif // CPGDB_ADJACENCYROW_HPP
