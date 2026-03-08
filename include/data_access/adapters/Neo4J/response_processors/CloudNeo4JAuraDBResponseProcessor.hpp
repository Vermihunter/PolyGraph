//
// Created by akos on 7. 3. 2026.
//

#ifndef CPGDB_CLOUDNEO4JAURADBRESPONSEPROCESSOR_HPP
#define CPGDB_CLOUDNEO4JAURADBRESPONSEPROCESSOR_HPP

#include "Neo4JResponseProcessor.hpp"

// Currently unused
class CloudNeo4JAuraDBResponseProcessor : public Neo4JResponseProcessor {
public:
    query_result_t process(const json &, PARSE_TYPE) override {

        return nullptr;
    }
};

#endif //CPGDB_CLOUDNEO4JAURADBRESPONSEPROCESSOR_HPP