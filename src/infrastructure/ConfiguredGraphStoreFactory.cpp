/**
 * @file src/infrastructure/ConfiguredGraphStoreFactory.cpp
 * @brief Implementation for configured graph store factory in the backend infrastructure module.
 */

#include "infrastructure/ConfiguredGraphStoreFactory.hpp"

#include "cli/BackendConfigParser.hpp"
#include "data_access/adapters/ArangoDB/ArangoGraphStore.hpp"
#include "data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp"
#include "data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp"
#include "data_access/adapters/Neo4J/Neo4jGraphStore.hpp"

#include <stdexcept>
#include <utility>
#ifdef CPGDB_ENABLE_GPU
#include "data_access/adapters/cuGraph/CuGraphProjectionStore.hpp"
#include "gpu/GpuRuntime.hpp"
#include "gpu/data_model/GpuProjectionBuilder.hpp"
#endif
#include "infrastructure/DatabaseContext.hpp"

namespace cpgdb {

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_from_json(DatabaseContext &context,
                                                                               const json &backend_json,
                                                                               const Inputs &inputs) {
        if (!config::backend_json_enabled(backend_json)) {
            return nullptr;
        }

        const BackendId id = config::backend_id_from_string(backend_json.at("id").get<std::string>());
        const BackendKind kind =
                config::backend_kind_from_string(backend_json.value("kind", config::backend_id_to_string(id)));

        auto connection = config::parse_connection_config(backend_json, id, kind);
        const auto descriptor_value =
                config::parse_descriptor(backend_json, id, kind, inputs.schema, inputs.nodes_by_backend,
                                         inputs.edges_by_backend, inputs.graph_version);

        context.register_backend_config(std::move(connection));
        const BackendConnectionConfig &stored_connection = context.config_for(id);

        switch (kind) {
            case BackendKind::Neo4j:
                return create_neo4j(context, descriptor_value, stored_connection);
            case BackendKind::Arango:
                return create_arango(context, descriptor_value, stored_connection);
            case BackendKind::CuGraph:
                return create_cugraph(context, descriptor_value, inputs);
            default:
                throw std::runtime_error("No IGraphStore factory registered for backend kind: " +
                                         config::backend_kind_to_string(kind));
        }
    }

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_neo4j(DatabaseContext &context,
                                                                           const BackendDescriptor &descriptor_value,
                                                                           const BackendConnectionConfig &connection) {
        auto visitor = std::make_shared<Neo4JQueryVisitor>(context, connection);
        return std::make_shared<Neo4jGraphStore>(context, descriptor_value, std::move(visitor));
    }

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_arango(DatabaseContext &context,
                                                                            const BackendDescriptor &descriptor_value,
                                                                            const BackendConnectionConfig &connection) {
        auto visitor = std::make_shared<ArangoQueryVisitor>(context, connection);
        return std::make_shared<ArangoGraphStore>(descriptor_value, std::move(visitor));
    }

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_cugraph(DatabaseContext &context,
                                                                             const BackendDescriptor &descriptor_value,
                                                                             const Inputs &inputs) {
#ifdef CPGDB_ENABLE_GPU
        if (inputs.ensure_cuda) {
            inputs.ensure_cuda();
        }
        if (!inputs.build_gpu_projection_inputs) {
            throw std::runtime_error("CuGraph backend requires build_gpu_projection_inputs factory input");
        }

        auto build = inputs.build_gpu_projection_inputs(inputs.data_path, context.schema);
        using Vertex = std::int32_t;
        using Edge = std::int32_t;
        using Projection = CsrGraphProjection<Vertex, Edge>;

        auto projection = GpuProjectionBuilder::from_records<Vertex, Edge>(
                context.gpu_runtime->handle, build.gpu_records, inputs.graph_version, true);

        return std::make_shared<CuGraphProjectionStore<Projection>>(descriptor_value, std::move(projection),
                                                                    std::move(build.host_index));
#else
        (void) context;
        (void) descriptor_value;
        (void) inputs;
        throw std::runtime_error("CuGraph backend requested, but this build was configured with CPGDB_ENABLE_GPU=OFF");
#endif
    }

} // namespace cpgdb
