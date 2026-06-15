//
// Created by akos on 16. 6. 2026.
//

#ifndef CPGDB_MINIMALADJACENCYROW_HPP
#define CPGDB_MINIMALADJACENCYROW_HPP

#include <vector>

#include "NodeRef.hpp"

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

#endif //CPGDB_MINIMALADJACENCYROW_HPP