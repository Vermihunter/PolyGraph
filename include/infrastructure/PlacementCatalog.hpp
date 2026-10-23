/**
 * @file include/infrastructure/PlacementCatalog.hpp
 * @brief Declares runtime selection of stores for native and distributed traversal.
 */

#ifndef POLYGRAPH_INFRASTRUCTURE_PLACEMENT_CATALOG_HPP
#define POLYGRAPH_INFRASTRUCTURE_PLACEMENT_CATALOG_HPP

#include "algorithms/bfs/BfsOptions.hpp"
#include "data_model/NodeRef.hpp"
#include "infrastructure/BackendRegistry.hpp"
#include "infrastructure/Placement.hpp"

#include <memory>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace polygraph {

/**
 * @brief Runtime store selection for native and ID-routed distributed traversal.
 *
 * Complete native BFS pushdown still uses backend capabilities/coverage. Normal
 * layer expansion decodes each frontier global ID using the configured routing layout.
 * The catalog deliberately does not keep an ID-to-type/backend directory: the
 * backend is encoded in the ID MSBs, while logical type resolution is delegated to
 * the owning store when a type is actually needed.
 */
struct CacheResidencyRecord {
    GlobalId canonical_id{};
    std::uint64_t mask{};
};

struct GpuOwnerRecord {
    GlobalId canonical_id{};
    std::uint32_t device_id{};
    std::uint32_t reserved{};
};

class PlacementCatalog {
  public:
    /** @brief Store-specific partitions of one logical BFS frontier. */
    struct LayerExpansionPlan {
        /// Vertices assigned to each backend for the next expansion request.
        std::unordered_map<BackendId, std::vector<NodeRef>> backend_frontiers;
    };

    /** @brief Creates a catalog backed by @p backend_registry. */
    explicit PlacementCatalog(std::shared_ptr<BackendRegistry> backend_registry);

    /** @brief Sets the ordered backend layout encoded in ID routing MSBs. */
    void set_id_routing_backends(std::vector<BackendId> routing_backends);
    /** Set cache residency supplied by the selected external cache plan. */
    void set_cache_residency(std::shared_ptr<const std::vector<CacheResidencyRecord>> residency);
    /** Set the physical GPU:N owner of canonical vertices whose logical owner is GPU. */
    void set_gpu_owner_distribution(std::shared_ptr<const std::vector<GpuOwnerRecord>> ownership);
    /**
     * @brief Creates a routing reference for @p id without a global ID directory.
     *
     * The returned logical type is unresolved. The ID routing MSBs are decoded and
     * validated immediately, so an invalid/disabled backend still fails fast.
     */
    [[nodiscard]] NodeRef node_ref(const GlobalId& id) const;
    /** @brief Returns the authoritative backend that owns a canonical vertex ID. */
    [[nodiscard]] BackendId owner_backend(GlobalId id) const;
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
     * @brief Chooses a complete backend capable of exact bottom-up/pull expansion.
     *
     * Partial caches are intentionally excluded: pull must scan the entire candidate
     * vertex universe or it can miss shortest-path discoveries.
     */
    [[nodiscard]] std::optional<BackendId> choose_pull_backend(
        const ExpandSpec& spec, const BfsOptions& options) const;

    /**
     * @brief Assigns every frontier vertex to the home store encoded in its global ID.
     */
    [[nodiscard]] LayerExpansionPlan plan_frontier_expansion(std::span<const NodeRef> frontier,
                                                             const ExpandSpec& spec,
                                                             const BfsOptions& options) const;

  private:
    /** @brief Decodes the stable logical owner and applies physical GPU distribution. */
    [[nodiscard]] BackendId authoritative_owner(GlobalId id) const;
    /** @brief Computes physical owner plus selected external cache residency. */
    [[nodiscard]] std::uint32_t effective_location_mask(GlobalId id) const;
    /** @brief Decodes and validates the home store for one vertex. */
    [[nodiscard]] std::vector<BackendId> choose_expansion_backends_for_vertex(
        const NodeRef& vertex, const ExpandSpec& spec, const BfsOptions& options) const;

    std::shared_ptr<BackendRegistry> backend_registry_;
    std::vector<BackendId> id_routing_backends_;
    std::shared_ptr<const std::vector<CacheResidencyRecord>> cache_residency_;
    std::shared_ptr<const std::vector<GpuOwnerRecord>> gpu_owner_distribution_;
    bool external_cache_residency_enabled_{false};
    std::vector<Placement> placements_;
};

} // namespace polygraph

#endif // POLYGRAPH_INFRASTRUCTURE_PLACEMENT_CATALOG_HPP
