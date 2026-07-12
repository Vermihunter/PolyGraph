/**
 * @file include/query/query_types/InsertBatchQuery.hpp
 * @brief Backend-neutral batch query for immutable node and edge insertions.
 */

#ifndef CPGDB_QUERY_QUERY_TYPES_INSERT_BATCH_QUERY_HPP
#define CPGDB_QUERY_QUERY_TYPES_INSERT_BATCH_QUERY_HPP

#include "insertion/InsertionItem.hpp"
#include "query/Query.hpp"

#include <vector>

namespace cpgdb {

/** @brief Visitor-dispatched write query containing a backend-grouped insertion batch. */
class InsertBatchQuery final : public Query {
  public:
    /** @brief Creates a query owning @p items. */
    explicit InsertBatchQuery(std::vector<insertion::InsertionItem> items);

    /** @brief Returns every immutable item in submission order. */
    [[nodiscard]] const std::vector<insertion::InsertionItem>& items() const noexcept;
    /** @brief Returns true when the query contains no items. */
    [[nodiscard]] bool empty() const noexcept;
    /** @brief Returns the number of contained items. */
    [[nodiscard]] std::size_t size() const noexcept;
    query_result_t accept(QueryVisitor& visitor) const override;
    [[nodiscard]] query_t clone() const override;

  private:
    std::vector<insertion::InsertionItem> items_;
};

} // namespace cpgdb

#endif // CPGDB_QUERY_QUERY_TYPES_INSERT_BATCH_QUERY_HPP
