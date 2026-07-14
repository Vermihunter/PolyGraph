/**
 * @file include/domain/graph/TypeId.hpp
 * @brief Declarations for type id in the logical graph schema module.
 */

//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_TYPEID_HPP
#define CPGDB_TYPEID_HPP

#include <cstdint>

namespace cpgdb {
using TypeId = std::uint32_t;

/**
 * Sentinel used when a caller supplies only a global UUID.
 *
 * Logical schema type IDs start at one, so zero can safely represent an
 * unresolved type. Backend routing must never depend on this value; the home
 * backend is decoded from the UUID suffix bits.
 */
inline constexpr TypeId unresolved_type_id = 0;

} // namespace cpgdb

#endif // CPGDB_TYPEID_HPP
