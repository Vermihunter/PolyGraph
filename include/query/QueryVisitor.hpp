#ifndef QUERY_VISITOR_HPP
#define QUERY_VISITOR_HPP

#include <memory>

#include <query/QueryResult.hpp>

//#include "engine/DatabaseContext.hpp"

class GetAllEdgesQuery;
class GetAllVerticesQuery;
class GetNeighborsForVertexQuery;
class GetVertexByIdQuery;

class DatabaseContext;

class QueryVisitor {
protected:
    DatabaseContext& db_context_;
public:
    virtual ~QueryVisitor() = default;

    QueryVisitor(DatabaseContext& db_context) : db_context_(db_context) {}

    virtual query_result_t visit(const GetAllEdgesQuery&) = 0;
    virtual query_result_t visit(const GetAllVerticesQuery&) = 0;
    virtual query_result_t visit(const GetNeighborsForVertexQuery&) = 0;
    virtual query_result_t visit(const GetVertexByIdQuery&) = 0;
};

using query_visitor_t = std::unique_ptr<QueryVisitor>;

#endif // QUERY_VISITOR_HPP