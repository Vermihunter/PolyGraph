#ifndef QUERY_HPP
#define QUERY_HPP

#include <memory>

#include <query/QueryVisitor.hpp>

struct Query;
using query_t = std::unique_ptr<Query>;

struct Query {
    virtual ~Query() = default;

    virtual query_result_t accept(QueryVisitor&) const = 0;
    virtual query_t clone() const = 0;
};



#endif // QUERY_HPP