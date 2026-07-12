/**
 * @file include/data_access/adapters/Neo4J/Neo4JBoltClient.hpp
 * @brief Declarations for neo4 jbolt client in the graph-store access module.
 */

#ifndef CPGDB_DATA_ACCESS_NEO4J_BOLT_CLIENT_HPP
#define CPGDB_DATA_ACCESS_NEO4J_BOLT_CLIENT_HPP

#include "utilities/json.hpp"

#include <memory>
#include <string>

namespace cpgdb {

class Neo4JBoltClient {
  public:
    Neo4JBoltClient(std::string endpoint,
                    std::string username,
                    std::string password,
                    std::string database_name);
    ~Neo4JBoltClient();

    Neo4JBoltClient(const Neo4JBoltClient&) = delete;
    Neo4JBoltClient& operator=(const Neo4JBoltClient&) = delete;

    [[nodiscard]] json run(const std::string& cypher, const json& parameters);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cpgdb

#endif // CPGDB_DATA_ACCESS_NEO4J_BOLT_CLIENT_HPP
