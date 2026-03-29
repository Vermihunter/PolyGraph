//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_NODE_HPP
#define CPGDB_NODE_HPP

#include <string>
#include <vector>

#include "Property.hpp"
#include "data_access/DatabaseTypes.hpp"

namespace domain {
    struct Node {
        std::string type;
        DatabaseTypes source;
        std::vector<Property> properties;
    };
}

#endif //CPGDB_NODE_HPP