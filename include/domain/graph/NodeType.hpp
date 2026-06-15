//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_NODE_HPP
#define CPGDB_NODE_HPP

#include <string>
#include <vector>

#include "Property.hpp"
#include "TypeId.hpp"
#include "../../infrastructure/BackendKind.hpp"

namespace domain {
    struct NodeType {
        TypeId id;
        std::string name;
        // DatabaseTypes source;
        std::vector<Property> properties;
    };
}

#endif //CPGDB_NODE_HPP