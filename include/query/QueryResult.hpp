/**
 * @file include/query/QueryResult.hpp
 * @brief Generic row-oriented result returned by visitor-based queries.
 */

#ifndef POLYGRAPH_QUERY_QUERY_RESULT_HPP
#define POLYGRAPH_QUERY_QUERY_RESULT_HPP

#include "data_model/Record.hpp"

#include <memory>
#include <vector>

namespace polygraph {

struct QueryResult;
/// Owning pointer used by query and visitor interfaces.
using query_result_t = std::unique_ptr<QueryResult>;

/** @brief Sequence of move-only heterogeneous records returned by a graph store. */
struct QueryResult {
    /// Result rows in backend/query-defined column order.
    std::vector<record_t> data;

    /** @brief Appends all rows from @p other, consuming the supplied result. */
    void combine(query_result_t other);
};

} // namespace polygraph

#endif // POLYGRAPH_QUERY_QUERY_RESULT_HPP
