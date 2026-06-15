//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_BACKENDDESCRIPTOR_HPP
#define CPGDB_BACKENDDESCRIPTOR_HPP
#include "BackendCapabilities.hpp"
#include "BackendId.hpp"
#include "BackendKind.hpp"
#include "GraphCoverage.hpp"

/**
 * Metadata about one logical backend: id, kind, capabilities, coverage, priority
 */
struct BackendDescriptor {
    BackendId id;
    BackendKind kind;
    BackendCapabilities capabilities;
    GraphCoverage coverage;

    // Smaller value = more preferred.
    // Example:
    //   GPU -> 0
    //   Memgraph -> 10
    //   Neo4j GDS -> 20
    //   Arango -> 30
    //   persistent fallback -> 100
    int priority = 100;

    std::string name;
};

#endif //CPGDB_BACKENDDESCRIPTOR_HPP