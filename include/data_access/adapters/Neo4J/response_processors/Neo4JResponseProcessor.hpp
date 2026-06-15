//
// Created by akos on 7. 3. 2026.
//

#ifndef CPGDB_NEO4JRESPONSEPROCESSOR_HPP
#define CPGDB_NEO4JRESPONSEPROCESSOR_HPP

#include <memory>

#include <query/QueryResult.hpp>
#include <utilities/json.hpp>

enum class PARSE_TYPE {
    NODE,
    RELATIONSHIP,
    ADJACENCY_ROW,
};

class Neo4JResponseProcessor {
public:
    Neo4JResponseProcessor() = default;
    virtual ~Neo4JResponseProcessor() = default;

    virtual query_result_t process(const json&, PARSE_TYPE parse_type) const = 0;
};

using neo_response_processor_t = std::unique_ptr<Neo4JResponseProcessor>;

#endif //CPGDB_NEO4JRESPONSEPROCESSOR_HPP
