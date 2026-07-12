/**
 * @file src/query/query_types/GetAllEdgesQuery.cpp
 * @brief Implementation for get all edges query in the query dispatch module.
 */

#include "query/query_types/GetAllEdgesQuery.hpp"

#include "query/QueryVisitor.hpp"

namespace cpgdb {
    query_result_t GetAllEdgesQuery::accept(QueryVisitor &visitor) const { return visitor.visit(*this); }
    query_t GetAllEdgesQuery::clone() const { return std::make_unique<GetAllEdgesQuery>(); }
} // namespace cpgdb
