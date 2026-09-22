/**
 * @file include/domain/graph/EdgeType.hpp
 * @brief Declarations for edge type in the logical graph schema module.
 */

//
// Created by akos on 29. 3. 2026.
//

#ifndef POLYGRAPH_EDGE_HPP
#define POLYGRAPH_EDGE_HPP

#include "../../infrastructure/BackendKind.hpp"
#include "Direction.hpp"
#include "NodeType.hpp"

namespace polygraph::domain {
struct EdgeType {
    TypeId type;
    TypeId from;
    TypeId to;
    // DatabaseTypes source;
    Direction direction;
    std::string name;
    std::vector<Property> properties;
};
} // namespace polygraph::domain

#endif // POLYGRAPH_EDGE_HPP