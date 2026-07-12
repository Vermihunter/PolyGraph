/**
 * @file include/infrastructure/BackendId.hpp
 * @brief Declarations for backend id in the backend infrastructure module.
 */

//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_BACKENDID_HPP
#define CPGDB_BACKENDID_HPP

#include <cstdint>

namespace cpgdb {
/**
 * Stable id of a logical backend, e.g. 1 = Neo4jMain, 2 = MemgraphHotCache
 */
using BackendId = std::uint32_t;

} // namespace cpgdb

#endif // CPGDB_BACKENDID_HPP
