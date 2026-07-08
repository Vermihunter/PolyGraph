//
// Created by akos on 17. 6. 2026.
//

#ifndef CPGDB_CONFIGUREDGRAPHSTOREFACTORY_HPP
#define CPGDB_CONFIGUREDGRAPHSTOREFACTORY_HPP

#include <cstdint>
#include <utility>
#include <vector>

#include "cli/BackendConfigParser.hpp"
#include "data_access/IGraphStore.hpp"
#include "data_access/adapters/Neo4J/Neo4jGraphStore.hpp"
#include "data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp"
#include "data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp"
#include "data_access/adapters/ArangoDB/ArangoGraphStore.hpp"
#include "data_access/adapters/cuGraph/CuGraphProjectionStore.hpp"
#include "data_model/Record.hpp"
#include "domain/graph/LogicalGraphSchema.hpp"
#include "gpu/data_model/GpuProjectionBuilder.hpp"
#include "gpu/data_model/HostAdjacencyIndex.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "infrastructure/BackendId.hpp"
#include "infrastructure/BackendKind.hpp"
#include "infrastructure/DatabaseContext.hpp"

class ConfiguredGraphStoreFactory {
public:
    using vertex_t = int32_t;
    using edge_t = int32_t;
    using GpuProjection = CsrGraphProjection<vertex_t, edge_t>;

    struct GpuProjectionInput {
        std::vector<record_t> gpu_records;
        std::shared_ptr<HostAdjacencyIndex> host_index;
    };

    struct Inputs {
        std::string data_path;
        const domain::LogicalGraphSchema& schema;
        const std::unordered_map<BackendId, std::unordered_set<TypeId>>& nodes_by_backend;
        const std::unordered_map<BackendId, std::unordered_set<TypeId>>& edges_by_backend;
        std::uint64_t graph_version = 0;

        std::function<GpuProjectionInput(const std::string&, const std::shared_ptr<domain::LogicalGraphSchema>&)>
            build_gpu_projection_inputs;

        std::function<void()> ensure_cuda;
    };

    [[nodiscard]] std::shared_ptr<IGraphStore>
    create_from_json(DatabaseContext& ctx,
                     const json& backend_json,
                     const Inputs& inputs) const {
        if (!cpgdb::config::backend_json_enabled(backend_json)) {
            return nullptr;
        }

        const BackendId id = cpgdb::config::backend_id_from_string(
            backend_json.at("id").get<std::string>()
        );

        const BackendKind kind = cpgdb::config::backend_kind_from_string(
            backend_json.value("kind", cpgdb::config::backend_id_to_string(id))
        );

        BackendConnectionConfig connection =
            cpgdb::config::parse_connection_config(backend_json, id, kind);

        BackendDescriptor descriptor =
            cpgdb::config::parse_descriptor(
                backend_json,
                id,
                kind,
                inputs.schema,
                inputs.nodes_by_backend,
                inputs.edges_by_backend,
                inputs.graph_version
            );

        ctx.register_backend_config(std::move(connection));
        const auto& stored_connection = ctx.config_for(id);

        switch (kind) {
            case BackendKind::Neo4j:
                return create_neo4j(ctx, descriptor, stored_connection);

            case BackendKind::Arango:
                return create_arango(ctx, descriptor, stored_connection);

            //case BackendKind::Memgraph:
              //  return create_memgraph(descriptor);

            case BackendKind::CuGraph:
                return create_cugraph(ctx, descriptor, inputs);

            default:
                throw std::runtime_error(
                    "No IGraphStore factory registered for backend kind: " +
                    cpgdb::config::backend_kind_to_string(kind)
                );
        }
    }

private:
    static std::shared_ptr<IGraphStore>
    create_neo4j(DatabaseContext& ctx,
                 const BackendDescriptor& descriptor,
                 const BackendConnectionConfig& connection) {
        std::shared_ptr<QueryVisitor> visitor =
            std::make_shared<Neo4JQueryVisitor>(ctx, connection);

        return std::make_shared<Neo4jGraphStore>(
            ctx,
            descriptor,
            visitor
        );
    }

    static std::shared_ptr<IGraphStore>
    create_arango(DatabaseContext& ctx,
                  const BackendDescriptor& descriptor,
                  const BackendConnectionConfig& connection) {
        std::shared_ptr<QueryVisitor> visitor =
            std::make_shared<ArangoQueryVisitor>(ctx, connection);

        return std::make_shared<ArangoGraphStore>(
            ctx,
            descriptor,
            visitor
        );
    }

    /*
    static std::shared_ptr<IGraphStore>
    create_memgraph(const BackendDescriptor& descriptor) {
        return std::make_shared<MemgraphGraphStore>(descriptor);
    }
    */

    static std::shared_ptr<IGraphStore>
    create_cugraph(DatabaseContext& ctx,
                   const BackendDescriptor& descriptor,
                   const Inputs& inputs) {
        if (inputs.ensure_cuda) {
            inputs.ensure_cuda();
        }

        if (!inputs.build_gpu_projection_inputs) {
            throw std::runtime_error(
                "CuGraph backend requires build_gpu_projection_inputs factory input"
            );
        }

        auto build = inputs.build_gpu_projection_inputs(
            inputs.data_path,
            ctx.schema
        );

        std::shared_ptr<GpuProjection> gpu_projection =
            GpuProjectionBuilder::from_records<vertex_t, edge_t>(
                ctx.gpu_runtime->handle,
                build.gpu_records,
                1,
                true
            );

        return std::make_shared<CuGraphProjectionStore<GpuProjection>>(
            descriptor,
            gpu_projection,
            build.host_index
        );
    }
};

#endif //CPGDB_CONFIGUREDGRAPHSTOREFACTORY_HPP
