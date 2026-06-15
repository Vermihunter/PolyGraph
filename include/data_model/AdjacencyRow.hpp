//
// Created by akos on 1. 5. 2026.
//

#ifndef CPGDB_ADJACENCYROW_HPP
#define CPGDB_ADJACENCYROW_HPP
#include "NeighborRef.hpp"
#include "Node.hpp"

struct AdjacencyRow {
    NodeRef source;
    std::vector<NeighborRef> neighbors;
};

#endif //CPGDB_ADJACENCYROW_HPP