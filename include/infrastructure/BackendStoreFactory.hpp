//
// Created by akos on 16. 6. 2026.
//

#ifndef CPGDB_BACKENDSTOREFACTORY_HPP
#define CPGDB_BACKENDSTOREFACTORY_HPP

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <functional>

#include "infrastructure/DatabaseContext.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"

#include "storage/IGraphStore.hpp"

#include "data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp"
#include "data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp"

#include "storage/Neo4jGraphStore.hpp"
#include "storage/ArangoGraphStore.hpp"
#include "storage/MemgraphGraphStore.hpp"

class BackendStoreFactory {
public:
    using FactoryFn = std::function<std::shared_ptr<IGraphStore>(
        DatabaseContext&,
        const BackendDescriptor&
    )>;

    BackendStoreFactory() {
        register_factory(BackendKind::Neo4j,
            [](DatabaseContext& ctx, const BackendDescriptor& descriptor) {
                const auto& connection = require_connection(ctx, descriptor.id);

                std::shared_ptr<QueryVisitor> visitor =
                    std::make_shared<Neo4JQueryVisitor>(ctx, connection);

                return std::make_shared<Neo4jGraphStore>(
                    ctx,
                    descriptor,
                    visitor
                );
            }
        );

        register_factory(BackendKind::ArangoDB,
            [](DatabaseContext& ctx, const BackendDescriptor& descriptor) {
                const auto& connection = require_connection(ctx, descriptor.id);

                std::shared_ptr<QueryVisitor> visitor =
                    std::make_shared<ArangoQueryVisitor>(ctx, connection);

                return std::make_shared<ArangoGraphStore>(
                    ctx,
                    descriptor,
                    visitor
                );
            }
        );

        /*
        register_factory(BackendKind::Memgraph,
            [](DatabaseContext&, const BackendDescriptor& descriptor) {
                return std::make_shared<MemgraphGraphStore>(descriptor);
            }
        );
        */
    }

    void register_factory(BackendKind kind, FactoryFn factory) {
        factories_[kind] = std::move(factory);
    }

    std::shared_ptr<IGraphStore>
    create(DatabaseContext& ctx, const BackendDescriptor& descriptor) const {
        auto it = factories_.find(descriptor.kind);

        if (it == factories_.end()) {
            throw std::runtime_error(
                "No graph store factory registered for backend kind: " +
                std::to_string(static_cast<int>(descriptor.kind))
            );
        }

        return it->second(ctx, descriptor);
    }

private:
    std::unordered_map<BackendKind, FactoryFn> factories_;

    static const BackendConnectionConfig&
    require_connection(DatabaseContext& ctx, BackendId backend_id) {
        auto it = ctx.connection_configs.find(backend_id);

        if (it == ctx.connection_configs.end()) {
            throw std::runtime_error(
                "Missing connection config for backend id: " +
                std::to_string(backend_id.value)
            );
        }

        return it->second;
    }
};

#endif //CPGDB_BACKENDSTOREFACTORY_HPP
