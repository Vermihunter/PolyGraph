/**
 * @file include/infrastructure/BackendKind.hpp
 * @brief Declarations for backend kind in the backend infrastructure module.
 */

#ifndef DATABASE_TYPES_HPP
#define DATABASE_TYPES_HPP

namespace cpgdb {
enum class BackendKind {
    Neo4j,
    Arango,
    Memgraph,
    RamCache,
    CuGraph,
    Janus,

    COUNT
};

} // namespace cpgdb

#endif // DATABASE_TYPES_HPP
