/**
 * @file src/query/query_types/InsertQuery.cpp
 * @brief Implementation for insert query in the query dispatch module.
 */

#include "query/query_types/InsertQuery.hpp"

#include "query/QueryVisitor.hpp"

#include <utility>

namespace cpgdb {

    InsertQuery::InsertQuery(insertion::InsertionItem item) : item_(std::move(item)) {}

    const insertion::InsertionItem &InsertQuery::item() const noexcept { return item_; }

    query_result_t InsertQuery::accept(QueryVisitor &visitor) const { return visitor.visit(*this); }

    query_t InsertQuery::clone() const { return std::make_unique<InsertQuery>(*this); }

} // namespace cpgdb
