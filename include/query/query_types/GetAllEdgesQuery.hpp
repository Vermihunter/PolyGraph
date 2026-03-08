#ifndef GET_ALL_EDGES_QUERY_HPP
#define GET_ALL_EDGES_QUERY_HPP

#include <query/Query.hpp>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>

class GetAllEdgesQuery : public Query {
public:
    query_result_t accept(QueryVisitor& visitor) const override {
        return visitor.visit(*this);
    }

    query_t clone() const override {
        return std::make_unique<GetAllEdgesQuery>();
    }
};

#endif // GET_ALL_EDGES_QUERY_HPP