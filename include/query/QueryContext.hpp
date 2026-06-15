#ifndef QUERY_CONTEXT_HPP
#define QUERY_CONTEXT_HPP

#include <query/Query.hpp>
#include "../infrastructure/BackendKind.hpp"

struct QueryContext {
    query_t query;
    BackendKind targetDB;
};

#endif // QUERY_CONTEXT_HPP