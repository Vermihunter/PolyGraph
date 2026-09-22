/**
 * @file include/data_model/AdjacencyRow.hpp
 * @brief Declarations for adjacency row in the graph data-model module.
 */

//
// Created by akos on 1. 5. 2026.
//

#ifndef POLYGRAPH_ADJACENCYROW_HPP
#define POLYGRAPH_ADJACENCYROW_HPP
#include "NeighborRef.hpp"
#include "Node.hpp"

namespace polygraph {
struct AdjacencyRow {
    NodeRef source;
    std::vector<NeighborRef> neighbors;
};

} // namespace polygraph

#endif // POLYGRAPH_ADJACENCYROW_HPP
