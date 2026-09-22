/**
 * @file include/infrastructure/BackendKind.hpp
 * @brief Declarations for backend kind in the backend infrastructure module.
 */

#ifndef DATABASE_TYPES_HPP
#define DATABASE_TYPES_HPP

namespace polygraph {
enum class BackendKind {
    Neo4j,
    Arango,
    PostgreSQL,
    Memgraph,
    RamCache,
    CuGraph,
    RedisCache,
    MemoryMapped,
    Janus,

    COUNT
};

} // namespace polygraph

#endif // DATABASE_TYPES_HPP
