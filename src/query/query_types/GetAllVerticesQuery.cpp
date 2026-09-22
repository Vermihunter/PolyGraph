/**
 * @file src/query/query_types/GetAllVerticesQuery.cpp
 * @brief Implementation for get all vertices query in the query dispatch module.
 */

#include "query/query_types/GetAllVerticesQuery.hpp"

#include "query/QueryVisitor.hpp"

namespace polygraph {
    query_result_t GetAllVerticesQuery::accept(QueryVisitor &visitor) const { return visitor.visit(*this); }
    query_t GetAllVerticesQuery::clone() const { return std::make_unique<GetAllVerticesQuery>(); }
} // namespace polygraph
