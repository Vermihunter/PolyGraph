/**
 * @file include/data_model/MinimalAdjacencyRow.hpp
 * @brief Declarations for minimal adjacency row in the graph data-model module.
 */

//
// Created by akos on 16. 6. 2026.
//

#ifndef POLYGRAPH_MINIMALADJACENCYROW_HPP
#define POLYGRAPH_MINIMALADJACENCYROW_HPP

#include "NodeRef.hpp"

#include <vector>

namespace polygraph {
/*
 * Minimal one-hop expansion result for graph algorithms.
 *
 * This intentionally does not carry EdgeRef, properties, labels, or full Node
 * objects. BFS/PageRank/WCC-style algorithms usually need only the reached
 * vertex references. Rich AdjacencyRow remains available for queries that need
 * edge metadata.
 */
struct MinimalAdjacencyRow {
    NodeRef source;
    std::vector<NodeRef> neighbors;
};

} // namespace polygraph

#endif // POLYGRAPH_MINIMALADJACENCYROW_HPP
