/**
 * @file src/infrastructure/DatabaseContext.cpp
 * @brief Implementation for database context in the backend infrastructure module.
 */

#include "infrastructure/DatabaseContext.hpp"

#include "gpu/GpuRuntime.hpp"

#include <stdexcept>
#include <utility>

namespace polygraph {

    std::shared_ptr<DatabaseContext> DatabaseContext::create_empty() {
        auto context = std::make_shared<DatabaseContext>();
        context->schema = std::make_shared<domain::LogicalGraphSchema>();
        context->backend_registry = std::make_shared<BackendRegistry>();
        context->placement_catalog = std::make_shared<PlacementCatalog>(context->backend_registry);
        context->backend_schedulers = std::make_shared<BackendSchedulerRegistry>();
        context->input_id_resolver = [](const GlobalId id) { return id; };
        return context;
    }

    void DatabaseContext::register_backend_config(BackendConnectionConfig config) {
        connection_configs.insert_or_assign(config.id, std::move(config));
    }

    void DatabaseContext::register_store(std::shared_ptr<IGraphStore> store_value) {
        backend_registry->register_store(std::move(store_value));
    }

    void DatabaseContext::register_placement(Placement placement) {
        placement_catalog->register_placement(std::move(placement));
    }

    GlobalId DatabaseContext::resolve_input_id(const GlobalId &id) const {
        return input_id_resolver ? input_id_resolver(id) : id;
    }

    NodeRef DatabaseContext::node_ref(const GlobalId &id) const {
        return placement_catalog->node_ref(resolve_input_id(id));
    }

    const BackendConnectionConfig &DatabaseContext::config_for(BackendId id) const { return connection_configs.at(id); }

} // namespace polygraph
