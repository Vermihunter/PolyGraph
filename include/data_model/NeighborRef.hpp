//
// Created by akos on 1. 5. 2026.
//

#ifndef CPGDB_NEIGHBORREF_HPP
#define CPGDB_NEIGHBORREF_HPP
#include <optional>

#include "EdgeRef.hpp"
#include "NodeRef.hpp"

struct NeighborRef {
    NodeRef vertex;
    std::optional<EdgeRef> via_edge;
};

#endif //CPGDB_NEIGHBORREF_HPP