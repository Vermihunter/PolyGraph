/**
 * @file include/query/QueryVisitor.hpp
 * @brief Visitor interface implemented by backend query translators.
 */

#ifndef POLYGRAPH_QUERY_QUERY_VISITOR_HPP
#define POLYGRAPH_QUERY_QUERY_VISITOR_HPP

#include "query/QueryResult.hpp"

#include <memory>

namespace polygraph {

class DatabaseContext;
class GetAllEdgesQuery;
class GetAllVerticesQuery;
class GetNeighborsForVertexQuery;
class GetVertexByIdQuery;
class GetNeighborsBatchQuery;
class InsertQuery;
class InsertBatchQuery;

/**
 * @brief Double-dispatch interface translating logical queries to one backend.
 *
 * Read operations are mandatory. Insert operations have default implementations
 * that report unsupported behavior, preserving source compatibility for
 * read-only stores.
 */
class QueryVisitor {
  public:
    /** @brief Binds the visitor to shared runtime metadata and services. */
    explicit QueryVisitor(DatabaseContext& database_context) noexcept;
    virtual ~QueryVisitor();

    /// Executes a request for every edge visible to the backend.
    virtual query_result_t visit(const GetAllEdgesQuery&) = 0;
    /// Executes a request for every vertex visible to the backend.
    virtual query_result_t visit(const GetAllVerticesQuery&) = 0;
    /// Executes a one-vertex adjacency request.
    virtual query_result_t visit(const GetNeighborsForVertexQuery&) = 0;
    /// Executes an exact vertex-ID lookup.
    virtual query_result_t visit(const GetVertexByIdQuery&) = 0;
    /// Executes a batched frontier adjacency request.
    virtual query_result_t visit(const GetNeighborsBatchQuery&) = 0;
    /// Executes a single immutable insertion item.
    virtual query_result_t visit(const InsertQuery&);
    /// Executes a backend-grouped batch of immutable insertion items.
    virtual query_result_t visit(const InsertBatchQuery&);

  protected:
    /// Non-owning runtime context; it must outlive this visitor.
    DatabaseContext& db_context_;
};

} // namespace polygraph

#endif // POLYGRAPH_QUERY_QUERY_VISITOR_HPP
