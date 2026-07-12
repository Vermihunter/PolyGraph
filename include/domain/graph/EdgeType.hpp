/**
 * @file include/domain/graph/EdgeType.hpp
 * @brief Declarations for edge type in the logical graph schema module.
 */

//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_EDGE_HPP
#define CPGDB_EDGE_HPP

#include "../../infrastructure/BackendKind.hpp"
#include "Direction.hpp"
#include "NodeType.hpp"

namespace cpgdb::domain {
struct EdgeType {
    TypeId type;
    TypeId from;
    TypeId to;
    // DatabaseTypes source;
    Direction direction;
    std::string name;
    std::vector<Property> properties;
};
} // namespace cpgdb::domain

#endif // CPGDB_EDGE_HPP