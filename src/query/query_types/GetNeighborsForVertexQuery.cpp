/**
 * @file src/query/query_types/GetNeighborsForVertexQuery.cpp
 * @brief Implementation for get neighbors for vertex query in the query dispatch module.
 */

#include "query/query_types/GetNeighborsForVertexQuery.hpp"

#include "query/QueryVisitor.hpp"

namespace cpgdb {
    GetNeighborsForVertexQuery::GetNeighborsForVertexQuery(Node &node) noexcept : node_(node) {}
    Node &GetNeighborsForVertexQuery::node() const noexcept { return node_; }
    query_result_t GetNeighborsForVertexQuery::accept(QueryVisitor &visitor) const { return visitor.visit(*this); }
    query_t GetNeighborsForVertexQuery::clone() const { return std::make_unique<GetNeighborsForVertexQuery>(node_); }
} // namespace cpgdb
