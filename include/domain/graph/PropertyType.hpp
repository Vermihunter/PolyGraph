/**
 * @file include/domain/graph/PropertyType.hpp
 * @brief Declarations for property type in the logical graph schema module.
 */

//
// Created by akos on 29. 3. 2026.
//

#ifndef POLYGRAPH_PROPERTYTYPE_HPP
#define POLYGRAPH_PROPERTYTYPE_HPP

namespace polygraph::domain {
enum class PropertyType {
    string,
    integer,
    boolean,
    _float,
    date,
    datetime // ...
};
}

#endif // POLYGRAPH_PROPERTYTYPE_HPP