#ifndef EXTERNAL_DATABASE_CONTEXT_HPP
#define EXTERNAL_DATABASE_CONTEXT_HPP

#include <memory>

#include <query/QueryVisitor.hpp>
#include <data_access/ExternalDatabaseConfig.hpp>

struct ExternalDatabaseContext {
    query_visitor_t queryVisitor;
    external_db_config_t config;
};

using external_db_ctx_t = std::unique_ptr<ExternalDatabaseContext>;

#endif // EXTERNAL_DATABASE_CONTEXT_HPP