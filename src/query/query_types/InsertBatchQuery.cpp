/**
 * @file src/query/query_types/InsertBatchQuery.cpp
 * @brief Implementation for insert batch query in the query dispatch module.
 */

#include "query/query_types/InsertBatchQuery.hpp"

#include "query/QueryVisitor.hpp"

#include <utility>

namespace polygraph {

    InsertBatchQuery::InsertBatchQuery(std::vector<insertion::InsertionItem> items) : items_(std::move(items)) {}

    const std::vector<insertion::InsertionItem> &InsertBatchQuery::items() const noexcept { return items_; }

    bool InsertBatchQuery::empty() const noexcept { return items_.empty(); }

    std::size_t InsertBatchQuery::size() const noexcept { return items_.size(); }

    query_result_t InsertBatchQuery::accept(QueryVisitor &visitor) const { return visitor.visit(*this); }

    query_t InsertBatchQuery::clone() const { return std::make_unique<InsertBatchQuery>(*this); }

} // namespace polygraph
