/**
 * @file src/cli/GraphRuntimeBuilder.cpp
 * @brief Implementation for graph runtime builder in the command-line and runtime configuration
 * module.
 */

//
// Created by akos on 7. 7. 2026.
//

#include "cli/GraphRuntimeBuilder.hpp"
#include "utilities/uuid.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef CPGDB_ENABLE_GPU
#include <cuda_runtime_api.h>
#endif

#include "cli/BackendConfigParser.hpp"
#include "cli/GraphDataReader.hpp"
#include "domain/graph/TypeId.hpp"
#include "infrastructure/BackendId.hpp"
#include "infrastructure/ConfiguredGraphStoreFactory.hpp"
#include "infrastructure/PlacementCatalog.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "serialization/RecordJsonSerializer.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/json.hpp"

using json = nlohmann::json;

namespace cpgdb::cli {
    namespace {

        using ProjectionBuildResult = ConfiguredGraphStoreFactory::GpuProjectionInput;

        struct LoadedSchema {
            std::unique_ptr<domain::LogicalGraphSchema> schema;
            std::unordered_map<BackendId, std::unordered_set<TypeId>> nodes_by_backend;
            std::unordered_map<BackendId, std::unordered_set<TypeId>> edges_by_backend;
        };

        [[nodiscard]] const json &required_array(const json &object, std::string_view key) {
            const auto it = object.find(std::string(key));
            if (it == object.end() || !it->is_array()) {
                throw std::runtime_error("Expected JSON array field: " + std::string(key));
            }
            return *it;
        }

        [[nodiscard]] json read_json_file(const std::string &path) {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("Could not open JSON file: " + path);
            }
            return json::parse(input);
        }

        [[nodiscard]] GlobalId json_global_id(const json &value) {
            if (value.is_number_unsigned()) {
                return value.get<GlobalId>();
            }
            if (value.is_number_integer()) {
                return global_id_from_signed_storage(value.get<std::int64_t>());
            }
            if (value.is_string()) {
                return global_id_from_string(value.get<std::string>());
            }
            throw std::runtime_error("Global ID must be an integer or decimal string");
        }

        [[nodiscard]] domain::PropertyType property_type_from_string(const std::string &value) {
            static const std::unordered_map<std::string, domain::PropertyType> by_name{
                    {"string", domain::PropertyType::string},   {"integer", domain::PropertyType::integer},
                    {"boolean", domain::PropertyType::boolean}, {"float", domain::PropertyType::_float},
                    {"date", domain::PropertyType::date},       {"datetime", domain::PropertyType::datetime},
            };

            const auto it = by_name.find(value);
            if (it == by_name.end()) {
                throw std::runtime_error("Unknown property type: " + value);
            }
            return it->second;
        }

        [[nodiscard]] std::vector<domain::Property> parse_properties(const json &properties_json) {
            std::vector<domain::Property> properties;

            for (const auto &property_json: properties_json) {
                properties.push_back(domain::Property{
                        .name = property_json.at("name").get<std::string>(),
                        .type = property_type_from_string(property_json.at("type").get<std::string>())});
            }

            return properties;
        }

        void ensure_cuda_or_throw() {
#ifdef CPGDB_ENABLE_GPU
            int device_count = 0;

            cudaError_t error = cudaGetDeviceCount(&device_count);
            if (error != cudaSuccess) {
                throw std::runtime_error(std::string("cudaGetDeviceCount failed: ") + cudaGetErrorString(error));
            }
            if (device_count == 0) {
                throw std::runtime_error("No CUDA devices available");
            }

            error = cudaSetDevice(0);
            if (error != cudaSuccess) {
                throw std::runtime_error(std::string("cudaSetDevice failed: ") + cudaGetErrorString(error));
            }

            error = cudaFree(nullptr);
            if (error != cudaSuccess) {
                throw std::runtime_error(std::string("CUDA context initialization failed: ") +
                                         cudaGetErrorString(error));
            }
#else
            throw std::runtime_error("GPU backend requested, but this build was configured with CPGDB_ENABLE_GPU=OFF");
#endif
        }

        [[nodiscard]] ProjectionBuildResult
        build_host_and_gpu_projection_inputs(const std::string &filename,
                                             const std::shared_ptr<domain::LogicalGraphSchema> &schema,
                                             const std::vector<BackendId> &routing_backends) {
            ProjectionBuildResult result;
            result.host_index = std::make_shared<HostAdjacencyIndex>();

            std::unordered_map<GlobalId, NodeRef, std::hash<GlobalId>> nodes_by_id;

            GraphDataReader::for_each(
                    filename,
                    [&](const json &node_json) {
                        const auto global_id = json_global_id(node_json.at("uuid"));
                        const auto type_name = node_json.at("type").get<std::string>();
                        const TypeId type_id = schema->node_type_id(type_name);
                        NodeRef ref{.global_id = global_id, .type = type_id};

                        nodes_by_id.emplace(global_id, ref);

                        if (insertion::BackendEncodedId::decode(global_id, routing_backends) == GPU) {
                            auto record = std::make_unique<Record>();
                            record->fields.emplace_back(
                                    Node{.global_id = global_id,
                                         .type = type_id,
                                         .labels = {type_name},
                                         .properties = node_json.value("properties", json::object()).get<Map>()});
                            result.gpu_records.push_back(std::move(record));
                        }
                    },
                    [&](const json &rel_json) {
                        const GlobalId relationship_id = json_global_id(rel_json.at("uuid"));
                        const auto rel_type_name = rel_json.at("type").get<std::string>();
                        const GlobalId from_id = json_global_id(rel_json.at("from"));
                        const GlobalId to_id = json_global_id(rel_json.at("to"));

                        if (!nodes_by_id.contains(from_id) || !nodes_by_id.contains(to_id)) {
                            throw std::runtime_error(
                                    "Relationship references an unknown endpoint. JSONL inputs must put all nodes "
                                    "before relationships. Relationship id=" +
                                    global_id_to_string(relationship_id));
                        }
                        const NodeRef &from_ref = nodes_by_id.at(from_id);
                        const NodeRef &to_ref = nodes_by_id.at(to_id);

                        EdgeRef edge_ref{.global_id = relationship_id,
                                         .type = schema->edge_type_id(rel_type_name),
                                         .from = from_ref.global_id,
                                         .to = to_ref.global_id};
                        result.host_index->add_edge(from_ref, to_ref, edge_ref);

                        const bool gpu_resident =
                                insertion::BackendEncodedId::decode(edge_ref.global_id, routing_backends) == GPU ||
                                insertion::BackendEncodedId::decode(from_id, routing_backends) == GPU ||
                                insertion::BackendEncodedId::decode(to_id, routing_backends) == GPU;
                        if (gpu_resident) {
                            auto record = std::make_unique<Record>();
                            record->fields.emplace_back(
                                    Edge{.global_id = edge_ref.global_id,
                                         .type = edge_ref.type,
                                         .src = from_ref.global_id,
                                         .dst = to_ref.global_id,
                                         .properties = rel_json.value("properties", json::object()).get<Map>()});
                            result.gpu_records.push_back(std::move(record));
                        }
                    });

            return result;
        }

        [[nodiscard]] LoadedSchema load_logical_graph_schema(const std::string &filename) {
            const json config = read_json_file(filename);

            LoadedSchema result;
            result.schema = std::make_unique<domain::LogicalGraphSchema>();

            // Keep your existing synthetic type.
            result.schema->add_node_type("Proxy", {});

            for (const auto &node_json: required_array(config, "nodeTypes")) {
                const auto name = node_json.at("name").get<std::string>();
                result.schema->add_node_type(name, parse_properties(node_json.value("properties", json::array())));
            }

            for (const auto &relation_json: required_array(config, "relations")) {
                const auto name = relation_json.at("name").get<std::string>();
                const TypeId from_id = result.schema->node_type_id(relation_json.at("from").get<std::string>());
                const TypeId to_id = result.schema->node_type_id(relation_json.at("to").get<std::string>());
                result.schema->add_edge_type(name, from_id, to_id, relation_json.value("directed", true),
                                             parse_properties(relation_json.value("properties", json::array())));
            }

            return result;
        }


        void validate_graph_id_layout(const std::string &filename, const std::vector<BackendId> &routing_backends) {
            const json metadata = GraphDataReader::metadata(filename);
            const auto layout = metadata.find("idRouting");
            if (layout == metadata.end()) {
                throw std::runtime_error(
                        "Graph metadata must declare idRouting; regenerate or migrate uint64 IDs first");
            }
            if (!layout->is_array()) {
                throw std::runtime_error("Graph metadata field 'idRouting' must be an array");
            }

            std::vector<BackendId> graph_routing;
            graph_routing.reserve(layout->size());
            for (const auto &value: *layout) {
                graph_routing.push_back(config::backend_id_from_string(value.get<std::string>()));
            }
            if (graph_routing != routing_backends) {
                throw std::runtime_error(
                        "Graph idRouting metadata does not match backend config; ID MSB prefixes would route to "
                        "the wrong storage layer");
            }

            if (const auto bits = metadata.find("idRoutingBits");
                bits != metadata.end() &&
                bits->get<std::size_t>() != insertion::BackendEncodedId::bit_count(routing_backends.size())) {
                throw std::runtime_error("Graph idRoutingBits metadata does not match backend count");
            }
        }

    } // namespace

    std::shared_ptr<DatabaseContext> GraphRuntimeBuilder::build(const GraphRuntimeConfig &config) {
        auto ctx = DatabaseContext::create_empty();

        LoadedSchema loaded_schema = load_logical_graph_schema(config.logical_graph_schema_path);

        ctx->schema = std::move(loaded_schema.schema);
        ctx->current_graph_version = config.graph_version;

        const json backend_root = config::load_backend_config_file_or_default(config.backend_config_path);
        const json &backend_entries = config::backend_array_or_throw(backend_root);
        ctx->id_routing_backends = config::id_routing_backends(backend_root);
        validate_graph_id_layout(config.data_path, ctx->id_routing_backends);
        ctx->placement_catalog->set_id_routing_backends(ctx->id_routing_backends);

        const auto projection_builder = [routing_backends = ctx->id_routing_backends](
                                                const std::string &path,
                                                const std::shared_ptr<domain::LogicalGraphSchema> &schema) {
            return build_host_and_gpu_projection_inputs(path, schema, routing_backends);
        };

        ConfiguredGraphStoreFactory::Inputs factory_inputs{.data_path = config.data_path,
                                                           .schema = *ctx->schema,
                                                           .nodes_by_backend = loaded_schema.nodes_by_backend,
                                                           .edges_by_backend = loaded_schema.edges_by_backend,
                                                           .graph_version = ctx->current_graph_version,
                                                           .build_gpu_projection_inputs = projection_builder,
                                                           .ensure_cuda = ensure_cuda_or_throw};

        std::size_t enabled_backend_count = 0;
        for (const auto &backend_json: backend_entries) {
            auto store = ConfiguredGraphStoreFactory::create_from_json(*ctx, backend_json, factory_inputs);
            if (!store) {
                continue;
            }

            ctx->register_store(std::move(store));
            ++enabled_backend_count;
        }

        if (enabled_backend_count == 0) {
            throw std::runtime_error("Backend config enables no backends");
        }

        if (config.verbose) {
            config::print_backend_summary(*ctx, std::cerr);
        }


        return ctx;
    }

} // namespace cpgdb::cli
