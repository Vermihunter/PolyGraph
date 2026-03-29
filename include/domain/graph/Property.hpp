//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_PROPERTY_HPP
#define CPGDB_PROPERTY_HPP

#include <string>

#include "PropertyType.hpp"

namespace domain {
    struct Property {
        std::string name;
        PropertyType type;
    };
}

#endif //CPGDB_PROPERTY_HPP