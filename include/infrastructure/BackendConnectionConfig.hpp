/**
 * @file include/infrastructure/BackendConnectionConfig.hpp
 * @brief Declarations for backend connection config in the backend infrastructure module.
 */

//
// Created by akos on 7. 5. 2026.
//

#ifndef POLYGRAPH_BACKENDCONNECTIONCONFIG_HPP
#define POLYGRAPH_BACKENDCONNECTIONCONFIG_HPP
#include "BackendId.hpp"
#include "BackendKind.hpp"
#include "DatabaseCredentials.hpp"

#include <optional>
#include <string>
#include <vector>

namespace polygraph {
struct BackendConnectionConfig {
    BackendId id;
    BackendKind kind;

    std::string name;

    std::string endpoint;
    std::string database_name;

    std::optional<DatabaseCredentials> credentials;

    // Neo4j can use either HTTP transaction API or Bolt.
    // transport: "http" or "bolt".  Environment variable
    // POLYGRAPH_NEO4J_TRANSPORT=bolt overrides this field.
    std::string transport = "http";
    std::string bolt_endpoint;

    // For Neo4j/Arango/Memgraph clusters or replicas
    std::vector<std::string> replica_endpoints;
};

} // namespace polygraph

#endif // POLYGRAPH_BACKENDCONNECTIONCONFIG_HPP
