/**
 * @file include/infrastructure/BackendId.hpp
 * @brief Declarations for backend id in the backend infrastructure module.
 */

//
// Created by akos on 30. 4. 2026.
//

#ifndef POLYGRAPH_BACKENDID_HPP
#define POLYGRAPH_BACKENDID_HPP

#include <cstdint>

namespace polygraph {
/**
 * Stable id of a logical backend, e.g. 1 = Neo4jMain, 2 = MemgraphHotCache
 */
using BackendId = std::uint32_t;

} // namespace polygraph

#endif // POLYGRAPH_BACKENDID_HPP
