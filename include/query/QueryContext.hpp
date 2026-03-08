#ifndef QUERY_CONTEXT_HPP
#define QUERY_CONTEXT_HPP

#include <query/Query.hpp>
#include <data_access/DatabaseTypes.hpp>

struct QueryContext {
    query_t query;
    DatabaseTypes targetDB;
};

#endif // QUERY_CONTEXT_HPP