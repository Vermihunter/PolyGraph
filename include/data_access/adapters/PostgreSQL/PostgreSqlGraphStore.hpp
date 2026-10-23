/**
 * @file include/data_access/adapters/PostgreSQL/PostgreSqlGraphStore.hpp
 * @brief PostgreSQL-backed graph store using a relational frontier join.
 */
#ifndef POLYGRAPH_POSTGRESQL_GRAPH_STORE_HPP
#define POLYGRAPH_POSTGRESQL_GRAPH_STORE_HPP

#include "data_access/IGraphStore.hpp"
#include "domain/graph/LogicalGraphSchema.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"

#include <libpq-fe.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace polygraph {

/**
 * Persistent relational graph backend.
 *
 * Large frontier expansion is intentionally set-oriented: PolyGraph COPYs the
 * current frontier once into a temporary PostgreSQL relation and joins the
 * edge rows against that relation. Persistent traversal indexes are optional.  The no-predecessor BFS path uses
 * libpq single-row mode and emits bounded candidate batches directly to BFS.
 */
class PostgreSqlGraphStore final : public IGraphStore {
  public:
    PostgreSqlGraphStore(BackendDescriptor descriptor,
                         BackendConnectionConfig connection,
                         const domain::LogicalGraphSchema& schema,
                         bool server_side_candidate_deduplication = false);
    ~PostgreSqlGraphStore() override;

    PostgreSqlGraphStore(const PostgreSqlGraphStore&) = delete;
    PostgreSqlGraphStore& operator=(const PostgreSqlGraphStore&) = delete;

    [[nodiscard]] BackendKind kind() const override { return BackendKind::PostgreSQL; }
    [[nodiscard]] const BackendDescriptor& descriptor() const override { return descriptor_; }

    std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth) override;

    std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                  const ExpandSpec& spec,
                                                                  int depth,
                                                                  const BfsOptions& options) override;

    void stream_neighbor_candidates(std::span<const NodeRef> frontier,
                                    const ExpandSpec& spec,
                                    int depth,
                                    const BfsOptions& options,
                                    const NeighborBatchConsumer& consumer) override;

  private:
    void ensure_connection_locked();
    void execute_command_locked(const std::string& sql);
    void begin_frontier_locked(std::span<const NodeRef> frontier);
    void finish_frontier_locked(bool commit) noexcept;

    [[nodiscard]] std::string build_candidate_query(const ExpandSpec& spec, bool include_source) const;
    [[nodiscard]] std::string edge_filter_sql(const ExpandSpec& spec, std::string_view alias) const;
    [[nodiscard]] std::string target_filter_sql(const ExpandSpec& spec, std::string_view column) const;
    [[nodiscard]] static std::string quote_literal(std::string_view value);
    [[nodiscard]] NodeRef parse_neighbor(PGresult* result, int id_column, int type_column) const;

    [[nodiscard]] std::size_t stream_query_locked(const std::string& sql,
                             std::size_t candidate_batch_size,
                             const NeighborBatchConsumer& consumer);

    BackendDescriptor descriptor_;
    BackendConnectionConfig connection_config_;
    const domain::LogicalGraphSchema& schema_;
    bool server_side_candidate_deduplication_{false};
    std::unordered_map<std::string, TypeId> node_type_ids_;

    mutable std::mutex mutex_;
    PGconn* connection_{nullptr};
};

} // namespace polygraph

#endif // POLYGRAPH_POSTGRESQL_GRAPH_STORE_HPP
