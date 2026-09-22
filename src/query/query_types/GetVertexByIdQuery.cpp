/**
 * @file src/query/query_types/GetVertexByIdQuery.cpp
 * @brief Implementation for get vertex by id query in the query dispatch module.
 */

#include "query/query_types/GetVertexByIdQuery.hpp"

#include "query/QueryVisitor.hpp"

namespace polygraph {
    GetVertexByIdQuery::GetVertexByIdQuery(Node &node) noexcept : node_(node) {}
    Node &GetVertexByIdQuery::node() const noexcept { return node_; }
    query_result_t GetVertexByIdQuery::accept(QueryVisitor &visitor) const { return visitor.visit(*this); }
    query_t GetVertexByIdQuery::clone() const { return std::make_unique<GetVertexByIdQuery>(node_); }
} // namespace polygraph
