//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_BACKENDID_HPP
#define CPGDB_BACKENDID_HPP

#include <cstdint>

/**
 * Stable id of a logical backend, e.g. 1 = Neo4jMain, 2 = MemgraphHotCache
 */
using BackendId = std::uint32_t;

#endif //CPGDB_BACKENDID_HPP