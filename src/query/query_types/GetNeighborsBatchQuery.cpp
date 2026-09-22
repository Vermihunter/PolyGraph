/**
 * @file src/query/query_types/GetNeighborsBatchQuery.cpp
 * @brief Implementation for get neighbors batch query in the query dispatch module.
 */

#include "query/query_types/GetNeighborsBatchQuery.hpp"

#include "query/QueryVisitor.hpp"

#include <utility>

namespace polygraph {
    GetNeighborsBatchQuery::GetNeighborsBatchQuery(std::vector<NodeRef> frontier, ExpandSpec spec) :
        frontier_(std::move(frontier)), spec_(std::move(spec)) {}
    const std::vector<NodeRef> &GetNeighborsBatchQuery::frontier() const noexcept { return frontier_; }
    const ExpandSpec &GetNeighborsBatchQuery::spec() const noexcept { return spec_; }
    query_result_t GetNeighborsBatchQuery::accept(QueryVisitor &visitor) const { return visitor.visit(*this); }
    query_t GetNeighborsBatchQuery::clone() const { return std::make_unique<GetNeighborsBatchQuery>(*this); }
} // namespace polygraph
