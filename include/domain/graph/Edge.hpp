//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_EDGE_HPP
#define CPGDB_EDGE_HPP

#include "data_access/DatabaseTypes.hpp"
#include "Node.hpp"

namespace domain {
    struct Edge {
        Node& from;
        Node& to;
        DatabaseTypes source;
        std::string type;
        std::vector<Property> properties;
    };
}
#endif //CPGDB_EDGE_HPP