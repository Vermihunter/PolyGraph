/**
 * @file include/insertion/InsertionPlacementPolicy.hpp
 * @brief Strategy interface and validated result for node and edge placement.
 */

#ifndef POLYGRAPH_INSERTION_INSERTION_PLACEMENT_POLICY_HPP
#define POLYGRAPH_INSERTION_INSERTION_PLACEMENT_POLICY_HPP

#include "data_model/Edge.hpp"
#include "data_model/Node.hpp"
#include "infrastructure/BackendId.hpp"
#include "infrastructure/BackendRegistry.hpp"
#include "insertion/BackendLoadTracker.hpp"

#include <optional>
#include <vector>

namespace polygraph::insertion {

/** @brief Physical write targets and optional logical ownership selected for one entity. */
struct PlacementDecision {
    /// Every backend that must receive the entity.
    std::vector<BackendId> target_backends;
    /// Logical owner; when present it must also appear in @ref target_backends.
    std::optional<BackendId> home_backend;
    /// Whether the coordinator must encode @ref home_backend into the entity ID.
    bool encode_home_backend_in_uuid = false;
};

/** @brief Read/write planning services exposed to a placement strategy. */
struct PlacementContext {
    /// Registry used to inspect candidates, capabilities, kind, and priority.
    const BackendRegistry& backend_registry;
    /// Per-batch committed-plus-projected load view.
    BackendLoadSnapshot& load_snapshot;
};

/**
 * @brief Runtime-polymorphic strategy for deciding node and edge write placement.
 *
 * Implementations may use type IDs, load, capabilities, priority, endpoint
 * locality, tenant metadata, geography, cost, or external policy state.
 */
class IInsertionPlacementPolicy {
  public:
    virtual ~IInsertionPlacementPolicy();

    /** @brief Selects physical targets and logical ownership for @p node. */
    [[nodiscard]] virtual PlacementDecision place_node(const Node& node,
                                                       PlacementContext& context) const = 0;
    /** @brief Selects physical targets and logical ownership for @p edge. */
    [[nodiscard]] virtual PlacementDecision place_edge(const Edge& edge,
                                                       PlacementContext& context) const = 0;
};

/**
 * @brief Validates non-empty, unique, registered targets and home-backend consistency.
 * @throws std::invalid_argument when the decision violates an invariant.
 */
void validate_placement_decision(const PlacementDecision& decision,
                                 const BackendRegistry& backend_registry);

} // namespace polygraph::insertion

#endif // POLYGRAPH_INSERTION_INSERTION_PLACEMENT_POLICY_HPP
