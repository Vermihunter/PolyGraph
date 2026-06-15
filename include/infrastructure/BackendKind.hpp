#ifndef DATABASE_TYPES_HPP
#define DATABASE_TYPES_HPP

enum class BackendKind {
    Neo4j,
    Arango,
    Memgraph,
    RamCache,
    CuGraph,
    Janus,

    COUNT
};

#endif // DATABASE_TYPES_HPP