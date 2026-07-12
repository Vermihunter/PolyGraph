/**
 * @file include/query/query_types/InsertQuery.hpp
 * @brief Backend-neutral query for one immutable node or edge insertion.
 */

#ifndef CPGDB_QUERY_QUERY_TYPES_INSERT_QUERY_HPP
#define CPGDB_QUERY_QUERY_TYPES_INSERT_QUERY_HPP

#include "insertion/InsertionItem.hpp"
#include "query/Query.hpp"

namespace cpgdb {

/** @brief Visitor-dispatched write query containing exactly one insertion item. */
class InsertQuery final : public Query {
  public:
    /** @brief Creates a query owning @p item. */
    explicit InsertQuery(insertion::InsertionItem item);

    /** @brief Returns the immutable insertion payload. */
    [[nodiscard]] const insertion::InsertionItem& item() const noexcept;
    query_result_t accept(QueryVisitor& visitor) const override;
    [[nodiscard]] query_t clone() const override;

  private:
    insertion::InsertionItem item_;
};

} // namespace cpgdb

#endif // CPGDB_QUERY_QUERY_TYPES_INSERT_QUERY_HPP
