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
 * @brief Runtime store selection for native and UUID-routed distributed traversal.
 *
 * Complete native BFS pushdown still uses backend capabilities/coverage. Normal
 * layer expansion decodes each frontier UUID using the configured routing layout.
 * The catalog deliberately does not keep a UUID-to-type/backend directory: the
 * backend is encoded in the UUID, while logical type resolution is delegated to
 * the owning store when a type is actually needed.
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

    /** @brief Sets the ordered backend layout encoded in UUID suffix bits. */
    void set_uuid_routing_backends(std::vector<BackendId> routing_backends);
    /**
     * @brief Creates a routing reference for @p id without a global UUID directory.
     *
     * The returned logical type is unresolved. The UUID suffix is decoded and
     * validated immediately, so an invalid/disabled backend still fails fast.
     */
    [[nodiscard]] NodeRef node_ref(const GlobalId& id) const;
    /** @brief Adds an optional replica/cache placement rule. */
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
     * @brief Assigns every frontier vertex to the home store encoded in its UUID.
     */
    [[nodiscard]] LayerExpansionPlan plan_frontier_expansion(std::span<const NodeRef> frontier,
                                                             const ExpandSpec& spec,
                                                             const BfsOptions& options) const;

  private:
    /** @brief Decodes and validates the home store for one vertex. */
    [[nodiscard]] std::vector<BackendId> choose_expansion_backends_for_vertex(
        const NodeRef& vertex, const ExpandSpec& spec, const BfsOptions& options) const;

    std::shared_ptr<BackendRegistry> backend_registry_;
    std::vector<BackendId> uuid_routing_backends_;
    std::vector<Placement> placements_;
};

} // namespace cpgdb

#endif // CPGDB_INFRASTRUCTURE_PLACEMENT_CATALOG_HPP
