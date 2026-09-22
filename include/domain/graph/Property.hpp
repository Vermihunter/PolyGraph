/**
 * @file include/domain/graph/Property.hpp
 * @brief Declarations for property in the logical graph schema module.
 */

//
// Created by akos on 29. 3. 2026.
//

#ifndef POLYGRAPH_PROPERTY_HPP
#define POLYGRAPH_PROPERTY_HPP

#include "PropertyType.hpp"

#include <string>

namespace polygraph::domain {
struct Property {
    std::string name;
    PropertyType type;
    bool indexed = false;
};
} // namespace polygraph::domain

#endif // POLYGRAPH_PROPERTY_HPP