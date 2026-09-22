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
#include "data_access/adapters/PostgreSQL/PostgreSqlGraphStore.hpp"
#include "data_access/adapters/cache/MMapGraphStore.hpp"
#include "data_access/adapters/cache/RedisGraphStore.hpp"
#include "utilities/BackendIds.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <stdexcept>
#include <utility>
#ifdef POLYGRAPH_ENABLE_GPU
#include "data_access/adapters/cuGraph/CuGraphProjectionStore.hpp"
#include "gpu/GpuRuntime.hpp"
#include "gpu/data_model/GpuProjectionBuilder.hpp"
#endif
#include "infrastructure/DatabaseContext.hpp"
#include "infrastructure/scheduling/BackendRequestScheduler.hpp"
#include "logging/Log.hpp"
#include "observability/MetricsAccess.hpp"

namespace polygraph {

    namespace {
        [[nodiscard]] BackendSchedulerConfig parse_scheduler_config(const json &backend_json) {
            BackendSchedulerConfig result;
            if (!backend_json.contains("scheduler")) {
                return result;
            }
            const auto &scheduler = backend_json.at("scheduler");
            if (!scheduler.is_object()) {
                throw std::runtime_error("backend.scheduler must be a JSON object");
            }

            result.max_in_flight = scheduler.value("max_in_flight", result.max_in_flight);
            result.max_queued = scheduler.value("max_queued", result.max_queued);
            result.max_queued_per_query = scheduler.value("max_queued_per_query", result.max_queued_per_query);
            result.priority_aging_ms = scheduler.value("priority_aging_ms", result.priority_aging_ms);
            result.policy =
                    parse_backend_scheduling_policy(scheduler.value("policy", std::string(to_string(result.policy))));

            if (result.max_in_flight == 0) {
                throw std::runtime_error("backend.scheduler.max_in_flight must be greater than zero");
            }
            return result;
        }

        class ProgressHeartbeat final {
        public:
            explicit ProgressHeartbeat(std::string message) :
                message_(std::move(message)), worker_([this] {
                    std::unique_lock lock(mutex_);
                    while (!stop_) {
                        if (wake_.wait_for(lock, std::chrono::seconds(5), [this] { return stop_; })) {
                            break;
                        }
                        lock.unlock();
                        LOG_INFO("{}", message_);
                        lock.lock();
                    }
                }) {}

            ~ProgressHeartbeat() {
                {
                    std::scoped_lock lock(mutex_);
                    stop_ = true;
                }
                wake_.notify_one();
                if (worker_.joinable()) {
                    worker_.join();
                }
            }

            ProgressHeartbeat(const ProgressHeartbeat &) = delete;
            ProgressHeartbeat &operator=(const ProgressHeartbeat &) = delete;

        private:
            std::string message_;
            std::mutex mutex_;
            std::condition_variable wake_;
            bool stop_{false};
            std::thread worker_;
        };
    } // namespace


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
                return create_neo4j(context, descriptor_value, stored_connection, backend_json);
            case BackendKind::Arango:
                return create_arango(context, descriptor_value, stored_connection, backend_json);
            case BackendKind::PostgreSQL:
                return create_postgresql(descriptor_value, stored_connection, inputs.schema, backend_json);
            case BackendKind::CuGraph:
                return create_cugraph(context, descriptor_value, backend_json, inputs);
            case BackendKind::RedisCache:
                return create_redis(descriptor_value, stored_connection, backend_json);
            case BackendKind::MemoryMapped:
                return create_mmap(descriptor_value, backend_json, inputs);
            default:
                throw std::runtime_error("No IGraphStore factory registered for backend kind: " +
                                         config::backend_kind_to_string(kind));
        }
    }

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_neo4j(DatabaseContext &context,
                                                                           const BackendDescriptor &descriptor_value,
                                                                           const BackendConnectionConfig &connection,
                                                                           const json &backend_json) {
        const auto scheduler = context.backend_schedulers->get_or_create(descriptor_value.id, descriptor_value.name,
                                                                         parse_scheduler_config(backend_json));
        auto visitor = std::make_shared<Neo4JQueryVisitor>(context, connection, scheduler);
        return std::make_shared<Neo4jGraphStore>(context, descriptor_value, std::move(visitor));
    }

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_arango(DatabaseContext &context,
                                                                            const BackendDescriptor &descriptor_value,
                                                                            const BackendConnectionConfig &connection,
                                                                            const json &backend_json) {
        const auto scheduler = context.backend_schedulers->get_or_create(descriptor_value.id, descriptor_value.name,
                                                                         parse_scheduler_config(backend_json));
        auto visitor = std::make_shared<ArangoQueryVisitor>(context, connection, scheduler);
        return std::make_shared<ArangoGraphStore>(descriptor_value, std::move(visitor));
    }

    std::shared_ptr<IGraphStore>
    ConfiguredGraphStoreFactory::create_postgresql(const BackendDescriptor &descriptor_value,
                                                   const BackendConnectionConfig &connection,
                                                   const domain::LogicalGraphSchema &schema, const json &backend_json) {
        if (connection.endpoint.empty()) {
            throw std::runtime_error("PostgreSQL backend requires connection.endpoint");
        }
        return std::make_shared<PostgreSqlGraphStore>(descriptor_value, connection, schema,
                                                      backend_json.value("server_side_candidate_deduplication", false));
    }

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_cugraph(DatabaseContext &context,
                                                                             const BackendDescriptor &descriptor_value,
                                                                             const json &backend_json,
                                                                             const Inputs &inputs) {
#ifdef POLYGRAPH_ENABLE_GPU
        const unsigned int device_id = backend_json.value("device_id", gpu_device_id_from_backend(descriptor_value.id));
        if (inputs.ensure_cuda) {
            inputs.ensure_cuda(device_id);
        }
        if (!inputs.build_gpu_projection_inputs) {
            throw std::runtime_error("CuGraph backend requires build_gpu_projection_inputs factory input");
        }

        auto build = inputs.build_gpu_projection_inputs(inputs.data_path, device_id);
        const auto adjacency_pairs = build.adjacency_pair_count();
        if (build.prebuilt_dense) {
            LOG_INFO("GPU:{} loading: reusable projection has {} resident vertices, {} projection vertices and "
                     "{} adjacency pairs",
                     device_id, build.resident_vertex_count, build.vertex_ids.size(), adjacency_pairs);
        } else {
            LOG_INFO("GPU:{} loading: legacy graph scan selected {} vertices and {} input edges", device_id,
                     build.vertex_ids.size(), adjacency_pairs);
        }
        const auto relation_limit = backend_json.value("relation_limit", std::uint64_t{0});
        if (relation_limit != 0U && adjacency_pairs > relation_limit) {
            throw std::runtime_error("GPU:" + std::to_string(device_id) +
                                     " projection exceeds configured relation_limit (" +
                                     std::to_string(adjacency_pairs) + " > " + std::to_string(relation_limit) + ")");
        }
        const auto memory_limit = backend_json.value("memory_limit_bytes", std::uint64_t{0});
        // New plans already budget peak construction memory conservatively. Keep a
        // cheap lower-bound guard here to reject obviously stale/mismatched artifacts.
        const auto planned_adjacency_bytes = static_cast<std::uint64_t>(adjacency_pairs) * 8U;
        if (memory_limit != 0U && planned_adjacency_bytes > memory_limit) {
            throw std::runtime_error("GPU:" + std::to_string(device_id) +
                                     " projection input alone exceeds configured memory_limit_bytes");
        }
        using Vertex = std::int32_t;
        using Edge = std::int32_t;
        using Projection = CsrGraphProjection<Vertex, Edge>;

        auto runtime = std::make_shared<GpuRuntime>();
        runtime->device_id = static_cast<int>(device_id);
        context.gpu_runtimes.insert_or_assign(descriptor_value.id, runtime);
        if (device_id == 0U) {
            context.gpu_runtime = runtime;
        }

        LOG_INFO("GPU:{} loading: building cuGraph CSR projection", device_id);
        std::shared_ptr<Projection> projection;
        {
            ProgressHeartbeat heartbeat("GPU:" + std::to_string(device_id) +
                                        " loading: cuGraph projection build still running");
            if (build.prebuilt_dense) {
                projection = GpuProjectionBuilder::from_prebuilt_dense_edgelist<Vertex, Edge>(
                        runtime->handle, std::move(build.vertex_ids), std::move(build.dense_sources),
                        std::move(build.dense_destinations), inputs.graph_version);
            } else {
                projection = GpuProjectionBuilder::from_edgelist<Vertex, Edge>(
                        runtime->handle, std::move(build.vertex_ids), std::move(build.edges), inputs.graph_version,
                        true);
            }
        }
        LOG_INFO("GPU:{} loading: cuGraph projection ready", device_id);

        const auto &stats = projection->stats();
        observability::set_gpu_projection_metrics(observability::GpuProjectionMetrics{
                .vertices = stats.vertex_count,
                .input_edges = stats.input_edge_count,
                .raw_adjacency_pairs = stats.raw_adjacency_pair_count,
                .unique_adjacency_pairs = stats.unique_adjacency_pair_count,
                .duplicate_adjacency_pairs_removed = stats.duplicate_adjacency_pair_count,
                .host_id_mapping_required_bytes = stats.host_id_mapping_required_bytes,
                .host_id_mapping_allocated_bytes = stats.host_id_mapping_allocated_bytes,
                .device_vertex_input_bytes = stats.device_vertex_input_bytes,
                .device_edge_input_bytes = stats.device_edge_input_bytes,
                .build_duration_seconds = stats.build_duration_seconds,
                .mirrored_for_undirected_expansion = stats.mirrored_for_undirected_expansion,
        });

        return std::make_shared<CuGraphProjectionStore<Projection>>(descriptor_value, std::move(projection));
#else
        (void) context;
        (void) descriptor_value;
        (void) backend_json;
        (void) inputs;
        throw std::runtime_error(
                "CuGraph backend requested, but this build was configured with POLYGRAPH_ENABLE_GPU=OFF");
#endif
    }

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_redis(const BackendDescriptor &descriptor_value,
                                                                           const BackendConnectionConfig &connection,
                                                                           const json &backend_json) {
        if (connection.endpoint.empty()) {
            throw std::runtime_error("RedisCache backend requires connection.endpoint");
        }
        int database = 0;
        if (!connection.database_name.empty()) {
            std::size_t consumed = 0;
            try {
                database = std::stoi(connection.database_name, &consumed);
            } catch (const std::exception &) {
                throw std::runtime_error("RedisCache connection.database must be a non-negative integer");
            }
            if (consumed != connection.database_name.size() || database < 0) {
                throw std::runtime_error("RedisCache connection.database must be a non-negative integer");
            }
        }
        return std::make_shared<RedisGraphStore>(descriptor_value, connection.endpoint, database,
                                                 backend_json.value("key_prefix", std::string{"polygraph:adj:"}),
                                                 backend_json.value("cache_plan_path", std::string{}));
    }

    std::shared_ptr<IGraphStore> ConfiguredGraphStoreFactory::create_mmap(const BackendDescriptor &descriptor_value,
                                                                          const json &backend_json,
                                                                          const Inputs &inputs) {
        const auto configured = backend_json.value("path", std::string{});
        if (configured.empty()) {
            throw std::runtime_error("MemoryMapped backend requires path");
        }
        std::filesystem::path path(configured);
        if (path.is_relative()) {
            path = std::filesystem::path(inputs.data_path).parent_path() / path;
        }
        return std::make_shared<MMapGraphStore>(descriptor_value, std::filesystem::weakly_canonical(path));
    }

} // namespace polygraph
