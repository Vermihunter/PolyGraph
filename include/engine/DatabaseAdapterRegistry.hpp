#ifndef DATABASE_ADAPTER_REGISTRY_HPP
#define DATABASE_ADAPTER_REGISTRY_HPP

#include <array>
#include <memory>

#include <data_access/IDatabase.hpp>
#include <data_access/DatabaseTypes.hpp>


class DatabaseAdapterRegistry {
    std::array<database_t, size_t(DatabaseTypes::COUNT)> registry;
        
public:
    void register_db(DatabaseTypes type, database_t dbAdapter) {
        registry[size_t(type)] = std::move(dbAdapter);
    }

    IDatabase * get(DatabaseTypes db) {
        return registry[size_t(db)].get();
    }
};

using db_adapter_registry_t = std::unique_ptr<DatabaseAdapterRegistry>;


#endif // DATABASE_ADAPTER_REGISTRY_HPP