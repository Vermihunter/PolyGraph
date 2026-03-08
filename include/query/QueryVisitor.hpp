#ifndef QUERY_VISITOR_HPP
#define QUERY_VISITOR_HPP

#include <memory>

#include <query/QueryResult.hpp>

class GetAllEdgesQuery;
class GetAllVerticesQuery;

class QueryVisitor {

public:
    virtual ~QueryVisitor() = default;

    virtual query_result_t visit(const GetAllEdgesQuery&) = 0;
    virtual query_result_t visit(const GetAllVerticesQuery&) = 0;
};

using query_visitor_t = std::unique_ptr<QueryVisitor>;

#endif // QUERY_VISITOR_HPP