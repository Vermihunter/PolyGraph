/**
 * @file
 * include/data_access/adapters/Neo4J/response_processors/LocallyDeployedNeo4JResponseProcessor.hpp
 * @brief Declarations for locally deployed neo4 jresponse processor in the graph-store access
 * module.
 */

#ifndef POLYGRAPH_DATA_ACCESS_NEO4J_LOCAL_RESPONSE_PROCESSOR_HPP
#define POLYGRAPH_DATA_ACCESS_NEO4J_LOCAL_RESPONSE_PROCESSOR_HPP

#include "data_access/adapters/Neo4J/response_processors/Neo4JResponseProcessor.hpp"

#include <memory>

namespace polygraph {

class DatabaseContext;

class LocallyDeployedNeo4JResponseProcessor final : public Neo4JResponseProcessor {
  public:
    explicit LocallyDeployedNeo4JResponseProcessor(DatabaseContext& database_context);
    ~LocallyDeployedNeo4JResponseProcessor() override;

    [[nodiscard]] query_result_t process(const json& response,
                                         ResultParseType parse_type) const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace polygraph

#endif // POLYGRAPH_DATA_ACCESS_NEO4J_LOCAL_RESPONSE_PROCESSOR_HPP
