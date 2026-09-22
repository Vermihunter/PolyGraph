/**
 * @file include/data_model/NeighborRef.hpp
 * @brief Declarations for neighbor ref in the graph data-model module.
 */

//
// Created by akos on 1. 5. 2026.
//

#ifndef POLYGRAPH_NEIGHBORREF_HPP
#define POLYGRAPH_NEIGHBORREF_HPP
#include "EdgeRef.hpp"
#include "NodeRef.hpp"

#include <optional>

namespace polygraph {
struct NeighborRef {
    NodeRef vertex;
    std::optional<EdgeRef> via_edge;
};

} // namespace polygraph

#endif // POLYGRAPH_NEIGHBORREF_HPP
