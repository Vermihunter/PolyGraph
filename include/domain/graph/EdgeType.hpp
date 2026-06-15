//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_EDGE_HPP
#define CPGDB_EDGE_HPP

#include "Direction.hpp"
#include "../../infrastructure/BackendKind.hpp"
#include "NodeType.hpp"

namespace domain {
    struct EdgeType {
        TypeId type;
        TypeId from;
        TypeId to;
        //DatabaseTypes source;
        Direction direction;
        std::string name;
        std::vector<Property> properties;
    };
}

#endif //CPGDB_EDGE_HPP