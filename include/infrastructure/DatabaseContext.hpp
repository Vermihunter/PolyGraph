#ifndef DATABASE_CONTEXT_HPP
#define DATABASE_CONTEXT_HPP

#include <memory>

#include <engine/DatabaseAdapterRegistry.hpp>

#include "QueryVisitorRegistry.hpp"
#include "data_access/IGraphStore.hpp"
#include "domain/graph/LogicalGraphSchema.hpp"
#include "gpu/GpuRuntime.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/BackendId.hpp"
#include "infrastructure/BackendRegistry.hpp"
#include "infrastructure/GraphVersion.hpp"
#include "infrastructure/PlacementCatalog.hpp"

struct DatabaseContext {
    // -----------------------------
    // Logical graph model
    // -----------------------------

    std::shared_ptr<domain::LogicalGraphSchema> schema;

    // -----------------------------
    // Physical setup
    // -----------------------------

    std::unordered_map<BackendId, BackendConnectionConfig> connection_configs;

    std::shared_ptr<BackendRegistry> backend_registry;
    std::shared_ptr<PlacementCatalog> placement_catalog;

    // -----------------------------
    // Query infrastructure
    // -----------------------------

    std::shared_ptr<QueryVisitorRegistry> query_visitors;

    // -----------------------------
    // Acceleration layers
    // -----------------------------

    // std::shared_ptr<GpuProjectionRegistry> gpu_projections;


    //std::shared_ptr<CacheRegistry> caches;

    // -----------------------------
    // Global state / policies
    // -----------------------------

    GraphVersion current_graph_version = 0;
    // Future point of expansion -> Read consistency / prefer GPU / frontier config (size / batch size) / allowed number of parallel requests
    // ExecutionPolicy execution_policy;

    std::shared_ptr<GpuRuntime> gpu_runtime;

    // -----------------------------
    // Construction helper
    // -----------------------------

    static auto create_empty() {
        auto ctx = std::make_shared<DatabaseContext>();

        ctx->schema = std::make_shared<domain::LogicalGraphSchema>();
        ctx->backend_registry = std::make_shared<BackendRegistry>();
        ctx->placement_catalog = std::make_shared<PlacementCatalog>(ctx->backend_registry);
        ctx->query_visitors = std::make_shared<QueryVisitorRegistry>();
        ctx->gpu_runtime = std::make_shared<GpuRuntime>();
        //ctx.gpu_projections = std::make_shared<GpuProjectionRegistry>();
        //ctx.caches = std::make_shared<CacheRegistry>();

        return ctx;
    }

    // -----------------------------
    // Convenience methods
    // -----------------------------

    void register_backend_config(BackendConnectionConfig config) {
        connection_configs[config.id] = std::move(config);
    }


    void register_store(std::shared_ptr<IGraphStore> store) {
        backend_registry->register_store(std::move(store));
    }


    void register_placement(Placement placement) {
        placement_catalog->register_placement(std::move(placement));
    }



    const BackendConnectionConfig& config_for(BackendId id) const {
        return connection_configs.at(id);
    }


    std::shared_ptr<IGraphStore> store(BackendId id) const {
        return backend_registry->get_store(id);
    }

    const BackendDescriptor& descriptor(BackendId id) const {
        return backend_registry->descriptor(id);
    }

    /*
    QueryContext make_query_context() const {
        return QueryContext {
            .required_graph_version = current_graph_version,
            .neo4j_bookmark = std::nullopt,
            .read_consistency = execution_policy.read_consistency
        };
    }
    */
};
using db_context_t = std::unique_ptr<DatabaseContext>;

#endif // DATABASE_CONTEXT_HPP