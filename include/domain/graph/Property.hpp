/**
 * @file include/domain/graph/Property.hpp
 * @brief Declarations for property in the logical graph schema module.
 */

//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_PROPERTY_HPP
#define CPGDB_PROPERTY_HPP

#include "PropertyType.hpp"

#include <string>

namespace cpgdb::domain {
struct Property {
    std::string name;
    PropertyType type;
    bool indexed = false;
};
} // namespace cpgdb::domain

#endif // CPGDB_PROPERTY_HPP