/**
 * @file include/infrastructure/PlacementCatalog.hpp
 * @brief Declares runtime selection of stores for native and distributed traversal.
 */

#ifndef CPGDB_INFRASTRUCTURE_PLACEMENT_CATALOG_HPP
#define CPGDB_INFRASTRUCTURE_PLACEMENT_CATALOG_HPP

#include "algorithms/bfs/BfsOptions.hpp"
#include "data_model/NodeRef.hpp"
#include "infrastructure/BackendRegistry.hpp"
#include "infrastructure/Placement.hpp"

#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace cpgdb {

/**
 * @brief Authoritative in-memory mapping from logical graph regions to stores.
 *
 * The catalog evaluates registered placements together with live backend
 * descriptors. It chooses complete native BFS pushdown when possible and
 * otherwise creates per-layer backend frontier partitions.
 */
class PlacementCatalog {
  public:
    /** @brief Store-specific partitions of one logical BFS frontier. */
    struct LayerExpansionPlan {
        /// Vertices assigned to each backend for the next expansion request.
        std::unordered_map<BackendId, std::vector<NodeRef>> backend_frontiers;
    };

    /** @brief Creates a catalog backed by @p backend_registry. */
    explicit PlacementCatalog(std::shared_ptr<BackendRegistry> backend_registry);

    /** @brief Adds a physical placement rule. */
    void register_placement(Placement placement);
    /** @brief Resolves @p id to a live graph store. */
    [[nodiscard]] std::shared_ptr<IGraphStore> store(BackendId id) const;

    /**
     * @brief Chooses the highest-priority backend that can answer the entire BFS.
     * @return Empty when native pushdown is disabled or no fresh complete placement exists.
     */
    [[nodiscard]] std::optional<BackendId> choose_native_bfs_backend(
        const NodeRef& source, const ExpandSpec& spec, const BfsOptions& options) const;

    /**
     * @brief Assigns every frontier vertex to one or more eligible expansion stores.
     *
     * Partial acceleration controls whether caches, replicas, and GPU projections
     * may be selected instead of authoritative primary placements.
     */
    [[nodiscard]] LayerExpansionPlan plan_frontier_expansion(std::span<const NodeRef> frontier,
                                                             const ExpandSpec& spec,
                                                             const BfsOptions& options) const;

  private:
    /** @brief Returns eligible stores for one vertex ordered by placement preference. */
    [[nodiscard]] std::vector<BackendId> choose_expansion_backends_for_vertex(
        const NodeRef& vertex, const ExpandSpec& spec, const BfsOptions& options) const;

    std::shared_ptr<BackendRegistry> backend_registry_;
    std::vector<Placement> placements_;
};

} // namespace cpgdb

#endif // CPGDB_INFRASTRUCTURE_PLACEMENT_CATALOG_HPP
