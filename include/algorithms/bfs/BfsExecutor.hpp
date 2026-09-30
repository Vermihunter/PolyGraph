/**
 * @file include/algorithms/bfs/BfsExecutor.hpp
 * @brief Declares the planner-driven distributed breadth-first-search executor.
 */

#ifndef POLYGRAPH_ALGORITHMS_BFS_EXECUTOR_HPP
#define POLYGRAPH_ALGORITHMS_BFS_EXECUTOR_HPP

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsLayer.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "algorithms/common/NeighborExpander.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "infrastructure/PlacementCatalog.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace polygraph {

/**
 * @brief Executes BFS against one logical graph distributed across registered stores.
 *
 * The executor first attempts full native pushdown through PlacementCatalog. When
 * pushdown is unavailable, it performs level-synchronous traversal, partitions
 * each frontier by backend, expands those partitions in parallel, and merges the
 * results while maintaining global discovery state.
 *
 * The catalog must outlive the executor.
 */
class BfsExecutor {
  public:
    /** @brief Creates an executor using @p catalog for all planning decisions. */
    explicit BfsExecutor(const PlacementCatalog& catalog) noexcept;

    /**
     * @brief Runs BFS and returns the complete materialized result.
     * @throws std::runtime_error when a selected backend expansion fails.
     */
    [[nodiscard]] BfsResult
    run(const NodeRef& source, const ExpandSpec& spec, const BfsOptions& options) const;

    /**
     * @brief Runs BFS and invokes @p on_layer after every completed depth.
     *
     * The callback executes synchronously on the calling thread after parallel
     * backend work for that layer has been merged.
     */
    [[nodiscard]] BfsResult
    run_streaming(const NodeRef& source,
                  const ExpandSpec& spec,
                  const BfsOptions& options,
                  const std::function<void(const BfsLayerEvent&)>& on_layer) const;

  private:
    /** @brief Compact per-run state that avoids ID-keyed predecessor storage. */
    struct DenseBfsState {
        /// Sentinel indicating that a dense vertex has no predecessor.
        static constexpr std::uint32_t no_predecessor = UINT32_MAX;

        /// Stable ID to dense-index lookup.
        std::unordered_map<GlobalId, std::uint32_t, std::hash<GlobalId>> to_dense;
        /// Reverse dense-index to ID lookup.
        std::vector<GlobalId> to_global;
        /// Distance for each dense vertex; negative values mean undiscovered.
        std::vector<int> distance;
        /// Dense predecessor index or @ref no_predecessor.
        std::vector<std::uint32_t> predecessor;

        /** @brief Reserves storage for approximately @p size discovered vertices. */
        void reserve(std::size_t size);
        /** @brief Returns the existing dense index or creates one for @p id. */
        [[nodiscard]] std::uint32_t get_or_add(const GlobalId& id);
    };

    /** @brief Materialized distributed fallback used by @ref run. */
    [[nodiscard]] BfsResult run_distributed_frontier_bfs(const NodeRef& source,
                                                         const ExpandSpec& spec,
                                                         const BfsOptions& options) const;

    /** @brief Streaming distributed fallback used by @ref run_streaming. */
    [[nodiscard]] BfsResult run_distributed_frontier_bfs_streaming(
        const NodeRef& source,
        const ExpandSpec& spec,
        const BfsOptions& options,
        const std::function<void(const BfsLayerEvent&)>& on_layer) const;

    /** @brief Candidate-streaming fallback used when predecessor edges are not requested. */
    [[nodiscard]] BfsResult run_distributed_frontier_bfs_candidates(
        const NodeRef& source,
        const ExpandSpec& spec,
        const BfsOptions& options,
        const std::function<void(const BfsLayerEvent&)>& on_layer) const;

    /**
     * @brief Adaptive distributed BFS using push/pull direction switching and sparse/bitmap frontiers.
     *
     * Exact pull is used only when PlacementCatalog exposes a complete pull-capable backend;
     * otherwise the policy automatically falls back to top-down push while retaining adaptive
     * frontier membership representation.
     */
    [[nodiscard]] BfsResult run_direction_optimized_bfs(
        const NodeRef& source,
        const ExpandSpec& spec,
        const BfsOptions& options,
        const std::function<void(const BfsLayerEvent&)>& on_layer) const;

    /** @brief Converts compact dense state to the external ID-keyed representation. */
    [[nodiscard]] static BfsResult make_result(const DenseBfsState& state,
                                               bool include_predecessors);

    /// Non-owning placement catalog used for store selection and lookup.
    const PlacementCatalog& catalog_;
    NeighborExpander expander_;
};

} // namespace polygraph

#endif // POLYGRAPH_ALGORITHMS_BFS_EXECUTOR_HPP
