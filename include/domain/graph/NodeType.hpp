/**
 * @file include/domain/graph/NodeType.hpp
 * @brief Declarations for node type in the logical graph schema module.
 */

//
// Created by akos on 29. 3. 2026.
//

#ifndef POLYGRAPH_NODE_HPP
#define POLYGRAPH_NODE_HPP

#include "../../infrastructure/BackendKind.hpp"
#include "Property.hpp"
#include "TypeId.hpp"

#include <string>
#include <vector>

namespace polygraph::domain {
struct NodeType {
    TypeId id;
    std::string name;
    // DatabaseTypes source;
    std::vector<Property> properties;
};
} // namespace polygraph::domain

#endif // POLYGRAPH_NODE_HPP