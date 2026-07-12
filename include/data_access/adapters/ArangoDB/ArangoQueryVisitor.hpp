/**
 * @file include/data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp
 * @brief Declarations for arango query visitor in the graph-store access module.
 */

#ifndef CPGDB_DATA_ACCESS_ARANGO_QUERY_VISITOR_HPP
#define CPGDB_DATA_ACCESS_ARANGO_QUERY_VISITOR_HPP

#include "infrastructure/BackendConnectionConfig.hpp"
#include "query/QueryVisitor.hpp"

#include <memory>

namespace cpgdb {

class ArangoQueryVisitor final : public QueryVisitor {
  public:
    ArangoQueryVisitor(DatabaseContext& database_context, const BackendConnectionConfig& config);
    ~ArangoQueryVisitor() override;

    query_result_t visit(const GetAllVerticesQuery& query) override;
    query_result_t visit(const GetAllEdgesQuery& query) override;
    query_result_t visit(const GetNeighborsForVertexQuery& query) override;
    query_result_t visit(const GetVertexByIdQuery& query) override;
    query_result_t visit(const GetNeighborsBatchQuery& query) override;
    query_result_t visit(const InsertQuery& query) override;
    query_result_t visit(const InsertBatchQuery& query) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cpgdb

#endif // CPGDB_DATA_ACCESS_ARANGO_QUERY_VISITOR_HPP
