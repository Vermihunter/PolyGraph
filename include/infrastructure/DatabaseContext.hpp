/**
 * @file include/infrastructure/DatabaseContext.hpp
 * @brief Shared runtime container for schema, stores, placements, and connections.
 */

#ifndef CPGDB_INFRASTRUCTURE_DATABASE_CONTEXT_HPP
#define CPGDB_INFRASTRUCTURE_DATABASE_CONTEXT_HPP

#include "data_access/IGraphStore.hpp"
#include "domain/graph/LogicalGraphSchema.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/BackendRegistry.hpp"
#include "infrastructure/GraphVersion.hpp"
#include "infrastructure/PlacementCatalog.hpp"

#include <memory>
#include <unordered_map>

namespace cpgdb {

struct GpuRuntime;

/**
 * @brief Aggregates the services and metadata shared by backend visitors and algorithms.
 *
 * Use @ref create_empty to obtain a context whose schema, registry, and placement
 * catalog are initialized consistently.
 */
struct DatabaseContext {
    /// Logical graph schema used to translate type IDs and property metadata.
    std::shared_ptr<domain::LogicalGraphSchema> schema;
    /// Connection settings indexed by backend ID.
    std::unordered_map<BackendId, BackendConnectionConfig> connection_configs;
    /// Active physical graph stores.
    std::shared_ptr<BackendRegistry> backend_registry;
    /// Authoritative runtime placement lookup and BFS planner input.
    std::shared_ptr<PlacementCatalog> placement_catalog;
    /// Current logical graph version used by coverage/projection freshness checks.
    GraphVersion current_graph_version = 0;
    /// Optional GPU runtime and projection state.
    std::shared_ptr<GpuRuntime> gpu_runtime;

    /** @brief Creates a context with initialized schema, registry, and catalog. */
    [[nodiscard]] static std::shared_ptr<DatabaseContext> create_empty();

    /** @brief Adds or replaces one backend connection configuration. */
    void register_backend_config(BackendConnectionConfig config);
    /** @brief Registers a live store in @ref backend_registry. */
    void register_store(std::shared_ptr<IGraphStore> store);
    /** @brief Adds a placement rule to @ref placement_catalog. */
    void register_placement(Placement placement);

    /** @brief Returns connection settings for @p id. */
    [[nodiscard]] const BackendConnectionConfig& config_for(BackendId id) const;
};

} // namespace cpgdb

#endif // CPGDB_INFRASTRUCTURE_DATABASE_CONTEXT_HPP
