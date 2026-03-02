#ifndef DATABASE_CONTEXT_HPP
#define DATABASE_CONTEXT_HPP

#include <memory>

#include <planner/QueryPlanner.hpp>
#include <engine/DatabaseAdapterRegistry.hpp>

struct DatabaseContext {
    query_planner_t planner;
    db_adapter_registry_t db_registry;
};

using db_context_t = std::unique_ptr<DatabaseContext>;

#endif // DATABASE_CONTEXT_HPP