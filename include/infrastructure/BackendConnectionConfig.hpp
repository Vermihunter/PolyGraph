//
// Created by akos on 7. 5. 2026.
//

#ifndef CPGDB_BACKENDCONNECTIONCONFIG_HPP
#define CPGDB_BACKENDCONNECTIONCONFIG_HPP
#include <optional>
#include <string>
#include <vector>

#include "BackendId.hpp"
#include "BackendKind.hpp"
#include "DatabaseCredentials.hpp"

struct BackendConnectionConfig {
    BackendId id;
    BackendKind kind;

    std::string name;

    std::string endpoint;
    std::string database_name;

    std::optional<DatabaseCredentials> credentials;

    // Neo4j can use either HTTP transaction API or Bolt.
    // transport: "http" or "bolt".  Environment variable
    // CPGDB_NEO4J_TRANSPORT=bolt overrides this field.
    std::string transport = "http";
    std::string bolt_endpoint;

    // For Neo4j/Arango/Memgraph clusters or replicas
    std::vector<std::string> replica_endpoints;
};


#endif //CPGDB_BACKENDCONNECTIONCONFIG_HPP