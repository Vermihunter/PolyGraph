/**
 * @file include/utilities/BackendIds.hpp
 * @brief Declarations for backend ids in the utility module.
 */

//
// Created by akos on 12. 5. 2026.
//

#ifndef CPGDB_BACKENDIDS_HPP
#define CPGDB_BACKENDIDS_HPP
#include "infrastructure/BackendId.hpp"

namespace cpgdb {
// -----------------------------
// Backend ids
// -----------------------------
constexpr BackendId GPU = 1;
constexpr BackendId MEMGRAPH = 2;
constexpr BackendId NEO4J = 3;
constexpr BackendId ARANGO = 4;

} // namespace cpgdb

#endif // CPGDB_BACKENDIDS_HPP
