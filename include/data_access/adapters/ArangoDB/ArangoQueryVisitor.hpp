/**
 * @file include/data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp
 * @brief Declarations for arango query visitor in the graph-store access module.
 */

#ifndef POLYGRAPH_DATA_ACCESS_ARANGO_QUERY_VISITOR_HPP
#define POLYGRAPH_DATA_ACCESS_ARANGO_QUERY_VISITOR_HPP

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "data_access/NeighborBatchConsumer.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/scheduling/BackendRequestScheduler.hpp"
#include "query/QueryVisitor.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace polygraph {

class ArangoQueryVisitor final : public QueryVisitor {
  public:
    ArangoQueryVisitor(DatabaseContext& database_context, const BackendConnectionConfig& config,
                       std::shared_ptr<BackendRequestScheduler> scheduler);
    ~ArangoQueryVisitor() override;

    query_result_t visit(const GetAllVerticesQuery& query) override;
    query_result_t visit(const GetAllEdgesQuery& query) override;
    query_result_t visit(const GetNeighborsForVertexQuery& query) override;
    query_result_t visit(const GetVertexByIdQuery& query) override;
    query_result_t visit(const GetNeighborsBatchQuery& query) override;
    query_result_t visit(const InsertQuery& query) override;
    query_result_t visit(const InsertBatchQuery& query) override;

    [[nodiscard]] std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_fast(
        std::span<const NodeRef> frontier,
        const ExpandSpec& spec,
        int depth,
        std::size_t batch_size,
        std::size_t max_concurrent_requests,
        ArangoRequestStrategy request_strategy,
        ArangoExpandMode expand_mode,
        bool use_plan_cache,
        std::size_t single_request_max_frontier,
        std::size_t request_retries,
        std::size_t retry_delay_ms,
        const BackendRequestScheduling& scheduling) const;

    void stream_neighbor_candidates_fast(
        std::span<const NodeRef> frontier,
        const ExpandSpec& spec,
        int depth,
        std::size_t batch_size,
        std::size_t max_concurrent_requests,
        ArangoRequestStrategy request_strategy,
        ArangoExpandMode expand_mode,
        bool use_plan_cache,
        std::size_t single_request_max_frontier,
        std::size_t request_retries,
        std::size_t retry_delay_ms,
        std::size_t candidate_batch_size,
        const NeighborBatchConsumer& consumer,
        const BackendRequestScheduling& scheduling) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace polygraph

#endif // POLYGRAPH_DATA_ACCESS_ARANGO_QUERY_VISITOR_HPP
