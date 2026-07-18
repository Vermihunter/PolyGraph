/**
 * @file include/data_access/adapters/Neo4J/Neo4jGraphStore.hpp
 * @brief Declarations for neo4j graph store in the graph-store access module.
 */

#ifndef CPGDB_DATA_ACCESS_NEO4J_GRAPH_STORE_HPP
#define CPGDB_DATA_ACCESS_NEO4J_GRAPH_STORE_HPP

#include "data_access/IGraphStore.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "query/QueryVisitor.hpp"

#include <memory>
#include <span>
#include <vector>

namespace cpgdb {

class Neo4jGraphStore final : public IGraphStore {
  public:
    Neo4jGraphStore(DatabaseContext& context,
                    BackendDescriptor descriptor,
                    std::shared_ptr<QueryVisitor> visitor);

    [[nodiscard]] BackendKind kind() const override;
    [[nodiscard]] const BackendDescriptor& descriptor() const override;
    query_result_t execute(const Query& query) override;
    std::vector<AdjacencyRow> get_neighbors_batch(std::span<const NodeRef> frontier,
                                                  const ExpandSpec& spec,
                                                  int depth) override;
    std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch(
        std::span<const NodeRef> frontier,
        const ExpandSpec& spec,
        int depth,
        const BfsOptions& options) override;
    BfsResult
    run_bfs(const NodeRef& source, const ExpandSpec& spec, const BfsOptions& options) override;

  private:
    [[nodiscard]] NodeRef node_to_ref(const Node& node) const;
    [[nodiscard]] static EdgeRef edge_to_ref(const Edge& edge);
    [[nodiscard]] std::vector<AdjacencyRow>
    parse_adjacency_rows(const QueryResult& raw, std::span<const NodeRef> frontier) const;
    [[nodiscard]] std::vector<AdjacencyRow>
    parse_neighbor_only_rows(const QueryResult& raw, std::span<const NodeRef> frontier) const;
    [[nodiscard]] std::vector<AdjacencyRow>
    parse_source_neighbor_rows(const QueryResult& raw) const;

    DatabaseContext& context_;
    BackendDescriptor descriptor_;
    std::shared_ptr<QueryVisitor> visitor_;
};

} // namespace cpgdb

#endif // CPGDB_DATA_ACCESS_NEO4J_GRAPH_STORE_HPP
