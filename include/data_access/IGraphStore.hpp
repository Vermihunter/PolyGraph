/**
 * @file include/data_access/IGraphStore.hpp
 * @brief Defines the backend-neutral graph store contract.
 */

#ifndef POLYGRAPH_DATA_ACCESS_I_GRAPH_STORE_HPP
#define POLYGRAPH_DATA_ACCESS_I_GRAPH_STORE_HPP

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "data_model/NodeRef.hpp"
#include "data_access/NeighborBatchConsumer.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "infrastructure/BackendKind.hpp"
#include "query/QueryResult.hpp"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace polygraph {

class Query;

/** @brief Membership predicate used by bottom-up BFS without exposing executor internals. */
using VertexMembershipPredicate = std::function<bool(GlobalId)>;

/** @brief One vertex discovered by a bottom-up/pull expansion and one valid BFS predecessor. */
struct PullCandidate {
    NodeRef vertex;
    NodeRef predecessor;
};

/** @brief Bounded batch consumer for pull discoveries. */
using PullCandidateConsumer = std::function<void(std::span<const PullCandidate>)>;

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
        std::span<const NodeRef> frontier,
        const ExpandSpec& spec,
        int depth,
        const BfsOptions& options);

    /**
     * @brief Streams one-hop neighbor candidates in bounded transient batches.
     *
     * This is the preferred path for BFS runs that do not need predecessor
     * information.  The default adapter flattens get_minimal_neighbors_batch(),
     * while high-volume backends override it so complete adjacency results never
     * need to exist in application memory.
     */
    virtual void stream_neighbor_candidates(
        std::span<const NodeRef> frontier,
        const ExpandSpec& spec,
        int depth,
        const BfsOptions& options,
        const NeighborBatchConsumer& consumer);

    /**
     * @brief Whether this store can perform an exact complete bottom-up scan for @p spec.
     *
     * Pull is deliberately stricter than normal one-hop expansion: returning true means the
     * store contains the complete candidate-vertex universe for the requested subgraph.
     */
    [[nodiscard]] virtual bool can_pull_expand(const ExpandSpec& spec) const;

    /** @brief Number of candidate vertices scanned by a complete pull expansion. */
    [[nodiscard]] virtual std::size_t pull_vertex_count() const;

    /**
     * @brief Streams vertices not in @p visited_contains that have any neighbor in the frontier.
     *
     * Every emitted item contains one frontier predecessor, preserving exact level-synchronous
     * BFS semantics and allowing predecessor maps to be produced in pull mode.
     */
    virtual void stream_pull_candidates(
        const ExpandSpec& spec,
        int depth,
        const BfsOptions& options,
        const VertexMembershipPredicate& frontier_contains,
        const VertexMembershipPredicate& visited_contains,
        const PullCandidateConsumer& consumer);

    /** @brief Returns whether this store can execute the complete requested BFS natively. */
    /** @brief Returns whether this store can answer one-hop expansion for this vertex/spec. */
    [[nodiscard]] virtual bool can_expand(const NodeRef& source, const ExpandSpec& spec) const;

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

} // namespace polygraph

#endif // POLYGRAPH_DATA_ACCESS_I_GRAPH_STORE_HPP
