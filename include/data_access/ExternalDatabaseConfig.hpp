#ifndef EXTERNAL_DATABASE_CONFIG_HPP
#define EXTERNAL_DATABASE_CONFIG_HPP

#include <memory>

struct ExternalDatabaseConfig {
    bool is_in_memory;
    unsigned capacity;
};

using external_db_config_t = std::unique_ptr<ExternalDatabaseConfig>;

#endif // EXTERNAL_DATABASE_CONFIG_HPP