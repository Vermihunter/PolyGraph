/**
 * @file include/data_access/adapters/ArangoDB/ArangoGraphStore.hpp
 * @brief Declarations for arango graph store in the graph-store access module.
 */

#ifndef CPGDB_DATA_ACCESS_ARANGO_GRAPH_STORE_HPP
#define CPGDB_DATA_ACCESS_ARANGO_GRAPH_STORE_HPP

#include "data_access/IGraphStore.hpp"
#include "query/QueryVisitor.hpp"

#include <memory>
#include <span>
#include <vector>

namespace cpgdb {

class ArangoGraphStore final : public IGraphStore {
  public:
    ArangoGraphStore(BackendDescriptor descriptor, std::shared_ptr<QueryVisitor> visitor);

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

  private:
    [[nodiscard]] static NodeRef node_to_ref(const Node& node);
    [[nodiscard]] static EdgeRef edge_to_ref(const Edge& edge);
    [[nodiscard]] static std::vector<AdjacencyRow> parse_adjacency_rows(const QueryResult& raw);

    BackendDescriptor descriptor_;
    std::shared_ptr<QueryVisitor> visitor_;
};

} // namespace cpgdb

#endif // CPGDB_DATA_ACCESS_ARANGO_GRAPH_STORE_HPP
