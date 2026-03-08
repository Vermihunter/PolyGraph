//
// Created by akos on 7. 3. 2026.
//

#ifndef CPGDB_NEO4JCONFIG_HPP
#define CPGDB_NEO4JCONFIG_HPP

#include <data_access/adapters/Neo4J/response_processors/Neo4JResponseProcessor.hpp>

struct Neo4JConfig{
    std::string endpoint;
    neo_response_processor_t response_processor;
};

#endif //CPGDB_NEO4JCONFIG_HPP