/**
 * @file include/data_access/IGraphStore.hpp
 * @brief Defines the backend-neutral graph store contract.
 */

#ifndef CPGDB_DATA_ACCESS_I_GRAPH_STORE_HPP
#define CPGDB_DATA_ACCESS_I_GRAPH_STORE_HPP

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "data_model/NodeRef.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "infrastructure/BackendKind.hpp"
#include "query/QueryResult.hpp"

#include <memory>
#include <span>
#include <vector>

namespace cpgdb {

class Query;

/**
 * @brief Polymorphic interface implemented by every physical graph backend.
 *
 * Stores expose optimized traversal operations as well as generic visitor-based
 * query execution. Default methods deliberately fail or adapt richer results so
 * a minimal backend only needs to implement its essential capabilities.
 */
class IGraphStore {
  public:
    virtual ~IGraphStore();

    /** @brief Returns the backend product/category implemented by this store. */
    [[nodiscard]] virtual BackendKind kind() const = 0;
    /** @brief Returns immutable capability, identity, priority, and coverage metadata. */
    [[nodiscard]] virtual const BackendDescriptor& descriptor() const = 0;

    /**
     * @brief Returns rich one-hop adjacency for all vertices in @p frontier.
     * @param depth Current BFS depth, available to cache-aware backends.
     */
    virtual std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth) = 0;

    /**
     * @brief Returns the lightweight adjacency form used by graph algorithms.
     *
     * The default implementation converts @ref get_neighbors_batch output.
     */
    virtual std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch(
        std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth);

    /** @brief Returns whether this store can execute the complete requested BFS natively. */
    [[nodiscard]] virtual bool can_run_native_bfs(const NodeRef& source,
                                                  const ExpandSpec& spec,
                                                  const BfsOptions& options) const;

    /**
     * @brief Executes a backend-neutral query through this store's query visitor.
     * @throws std::runtime_error when generic query execution is unsupported.
     */
    virtual query_result_t execute(const Query& query);

    /**
     * @brief Executes a complete native BFS.
     * @throws std::runtime_error when native BFS is unsupported.
     */
    virtual BfsResult
    run_bfs(const NodeRef& source, const ExpandSpec& spec, const BfsOptions& options);
};

} // namespace cpgdb

#endif // CPGDB_DATA_ACCESS_I_GRAPH_STORE_HPP
