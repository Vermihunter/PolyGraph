/**
 * @file include/data_access/adapters/Neo4J/response_processors/Neo4JResponseProcessor.hpp
 * @brief Declarations for neo4 jresponse processor in the graph-store access module.
 */

#pragma once

#include "query/QueryResult.hpp"
#include "query/ResultParseType.hpp"
#include "utilities/json.hpp"

namespace polygraph {

class Neo4JResponseProcessor {
  public:
    Neo4JResponseProcessor();
    virtual ~Neo4JResponseProcessor();

    Neo4JResponseProcessor(const Neo4JResponseProcessor&) = delete;
    Neo4JResponseProcessor& operator=(const Neo4JResponseProcessor&) = delete;

    [[nodiscard]] virtual query_result_t process(const json& response,
                                                 ResultParseType parse_type) const = 0;
};

} // namespace polygraph
