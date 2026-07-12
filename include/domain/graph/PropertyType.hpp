/**
 * @file include/domain/graph/PropertyType.hpp
 * @brief Declarations for property type in the logical graph schema module.
 */

//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_PROPERTYTYPE_HPP
#define CPGDB_PROPERTYTYPE_HPP

namespace cpgdb::domain {
enum class PropertyType {
    string,
    integer,
    boolean,
    _float,
    date,
    datetime // ...
};
}

#endif // CPGDB_PROPERTYTYPE_HPP