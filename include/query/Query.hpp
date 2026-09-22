/**
 * @file include/query/Query.hpp
 * @brief Base abstraction for backend-neutral graph operations.
 */

#ifndef QUERY_HPP
#define QUERY_HPP

#include "query/QueryResult.hpp"

#include <memory>

namespace polygraph {

struct Query;
/// Owning pointer to a polymorphic query object.
using query_t = std::unique_ptr<Query>;

class QueryVisitor;

/**
 * @brief Polymorphic command accepted by backend-specific QueryVisitor implementations.
 *
 * Queries are immutable after construction and cloneable so they can be queued,
 * copied across orchestration boundaries, or dispatched to multiple replicas.
 */
struct Query {
    virtual ~Query();

    /** @brief Dispatches this concrete query to @p visitor. */
    virtual query_result_t accept(QueryVisitor& visitor) const = 0;
    /** @brief Returns an owning copy preserving the concrete query type. */
    virtual query_t clone() const = 0;
};

} // namespace polygraph

#endif // QUERY_HPP
