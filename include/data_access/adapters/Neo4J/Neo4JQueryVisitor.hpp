/**
 * @file include/data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp
 * @brief Declarations for neo4 jquery visitor in the graph-store access module.
 */

#ifndef CPGDB_DATA_ACCESS_NEO4J_QUERY_VISITOR_HPP
#define CPGDB_DATA_ACCESS_NEO4J_QUERY_VISITOR_HPP

#include "algorithms/ExpandSpec.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "query/QueryVisitor.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace cpgdb {

class Neo4JQueryVisitor final : public QueryVisitor {
  public:
    Neo4JQueryVisitor(DatabaseContext& database_context, const BackendConnectionConfig& config);
    ~Neo4JQueryVisitor() override;

    query_result_t visit(const GetAllVerticesQuery& query) override;
    query_result_t visit(const GetAllEdgesQuery& query) override;
    query_result_t visit(const GetNeighborsForVertexQuery& query) override;
    query_result_t visit(const GetVertexByIdQuery& query) override;
    query_result_t visit(const GetNeighborsBatchQuery& query) override;
    query_result_t visit(const InsertQuery& query) override;
    query_result_t visit(const InsertBatchQuery& query) override;

    [[nodiscard]] std::vector<AdjacencyRow> get_neighbors_batch_fast(
        std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth) const;
    [[nodiscard]] std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_fast(
        std::span<const NodeRef> frontier,
        const ExpandSpec& spec,
        int depth,
        std::size_t batch_size,
        std::size_t max_concurrent_requests,
        std::size_t request_retries,
        std::size_t retry_delay_ms) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cpgdb

#endif // CPGDB_DATA_ACCESS_NEO4J_QUERY_VISITOR_HPP
