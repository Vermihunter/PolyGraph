#ifndef CPGDB_GETNEIGHBORSBATCHQUERY_HPP
#define CPGDB_GETNEIGHBORSBATCHQUERY_HPP

#include "query/QueryResult.hpp"
#include "query/QueryVisitor.hpp"
#include "algorithms/ExpandSpec.hpp"
#include "query/Query.hpp"

class GetNeighborsBatchQuery : public Query {
public:
    GetNeighborsBatchQuery(std::vector<NodeRef> frontier, ExpandSpec spec)
        : frontier_(std::move(frontier)), spec_(std::move(spec)) {}

    const std::vector<NodeRef>& frontier() const {
        return frontier_;
    }

    const ExpandSpec& spec() const {
        return spec_;
    }

    query_result_t accept(QueryVisitor& visitor) const override {
        return visitor.visit(*this);
    }

    [[nodiscard]] query_t clone() const override {
        return std::make_unique<GetNeighborsBatchQuery>(*this);
    }

private:
    std::vector<NodeRef> frontier_;
    ExpandSpec spec_;
};

#endif // CPGDB_GETNEIGHBORSBATCHQUERY_HPP