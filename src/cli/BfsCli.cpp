/**
 * @file src/cli/BfsCli.cpp
 * @brief Implementation for bfs cli in the command-line and runtime configuration module.
 */

//
// Created by akos on 7. 7. 2026.
//

#include "cli/BfsCli.hpp"
#include "cli/DomainManifest.hpp"
#include "utilities/uuid.hpp"

#include "events/Events.hpp"
#include "logging/LoggingSetup.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace polygraph::cli {
    namespace {

        [[nodiscard]] std::vector<std::string> split_csv(const std::string &value) {
            std::vector<std::string> result;
            std::string current;
            std::stringstream stream(value);

            while (std::getline(stream, current, ',')) {
                current.erase(current.begin(), std::find_if(current.begin(), current.end(),
                                                            [](unsigned char ch) { return !std::isspace(ch); }));
                current.erase(std::find_if(current.rbegin(), current.rend(),
                                           [](unsigned char ch) { return !std::isspace(ch); })
                                      .base(),
                              current.end());

                if (!current.empty()) {
                    result.push_back(current);
                }
            }

            return result;
        }

        [[nodiscard]] std::string default_thread_count() {
            const unsigned int detected = std::thread::hardware_concurrency();
            return std::to_string(detected == 0 ? 1U : detected);
        }

        [[nodiscard]] Direction parse_direction(const std::string &value) {
            if (value == "directed" || value == "Directed") {
                return Direction::Directed;
            }
            if (value == "undirected" || value == "Undirected") {
                return Direction::Undirected;
            }
            if (value == "both" || value == "Both") {
                return Direction::Both;
            }

            throw std::runtime_error("Unknown --direction value: " + value + " (expected directed, undirected, both)");
        }

        [[nodiscard]] std::optional<DomainManifest> configured_domain(const CommandLine &args) {
            const auto &path = args.string_value("domain");
            return path.empty() ? std::nullopt : std::optional<DomainManifest>{load_domain_manifest(path)};
        }

        [[nodiscard]] int parse_integer(const std::string &raw, const std::string_view option) {
            int value = 0;
            const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
            if (error != std::errc{} || end != raw.data() + raw.size()) {
                throw std::invalid_argument("Expected integer for " + std::string(option) + ": " + raw);
            }
            return value;
        }

        [[nodiscard]] std::vector<TypeId> parse_edge_type_filter(const std::string &csv,
                                                                 const domain::LogicalGraphSchema &schema) {
            std::vector<TypeId> result;
            for (const auto &name: split_csv(csv)) {
                result.push_back(schema.edge_type_id(name));
            }
            return result;
        }

        [[nodiscard]] std::vector<TypeId> parse_node_type_filter(const std::string &csv,
                                                                 const domain::LogicalGraphSchema &schema) {
            std::vector<TypeId> result;
            for (const auto &name: split_csv(csv)) {
                result.push_back(schema.node_type_id(name));
            }
            return result;
        }

    } // namespace

    void add_runtime_arguments(CommandLine &program) {
        program.add_value_option("domain", {"--domain"},
                                 "Path to domain.json containing schema, data, backend and observability paths",
                                 std::string{});

        program.add_value_option("logical-graph-schema", {"-lgs", "--logical-graph-schema"},
                                 "Compatibility override for domain.paths.schema", std::string{});

        program.add_value_option("data", {"--data"}, "Compatibility override for domain.paths.data", std::string{});

        program.add_value_option("backend-config", {"--backend-config"},
                                 "Compatibility override for domain.paths.backendConfig", std::string{});

        program.add_value_option("native-id-map", {"--native-id-map"},
                                 "Optional polygraph.native-id-map sidecar for resolving original input IDs",
                                 std::string{});

        program.add_value_option("cache-plan", {"--cache-plan"},
                                 "Selected cache-plan.json; overrides domain.paths.cachePlan", std::string{});

        program.add_flag_option("verbose", {"-v", "--verbose"}, "Enable verbose output");
    }

    void add_bfs_arguments(CommandLine &program) {
        program.add_value_option("output-file", {"--output-file"},
                                 "Write BFS distance CSV here instead of stdout (recommended when logging is enabled)",
                                 std::string{});

        program.add_value_option("start-uuid", {"--start-uuid"},
                                 "BFS source ID; defaults to domain.defaults.bfs.startUuid", std::string{});

        program.add_value_option("max-depth", {"--max-depth"},
                                 "BFS max depth; defaults to domain.defaults.bfs.maxDepth; use -1 for unlimited",
                                 std::string{});

        program.add_value_option("direction", {"--direction"},
                                 "BFS direction; defaults to domain.defaults.bfs.direction", std::string{});

        program.add_value_option("allowed-edge-types", {"--allowed-edge-types"},
                                 "Comma-separated logical edge type filter; empty means all", std::string{});

        program.add_value_option("allowed-target-node-types", {"--allowed-target-node-types"},
                                 "Comma-separated logical target node type filter; empty means all", std::string{});

        program.add_flag_option("allow-native-pushdown", {"--allow-native-pushdown"},
                                "Allow native backend BFS pushdown when possible");

        program.add_flag_option("no-partial-acceleration", {"--no-partial-acceleration"},
                                "Disable partial acceleration/projection usage");

        program.add_flag_option("no-predecessors", {"--no-predecessors"}, "Do not request predecessor information");

        program.add_value_option("neighbor-batch-size", {"--neighbor-batch-size"},
                                 "Maximum frontier vertices per Neo4j/ArangoDB neighbor request", std::string{"10000"});

        program.add_value_option("neo4j-max-concurrent-requests", {"--neo4j-max-concurrent-requests"},
                                 "Maximum number of concurrent Neo4j neighbor requests", std::string{"4"});

        program.add_value_option("arango-max-concurrent-requests", {"--arango-max-concurrent-requests"},
                                 "Maximum number of concurrent ArangoDB neighbor requests", std::string{"4"});

        program.add_value_option("arango-request-strategy", {"--arango-request-strategy"},
                                 "ArangoDB frontier request strategy: chunked, single, or auto",
                                 std::string{"chunked"});

        program.add_value_option(
                "arango-single-request-max-frontier", {"--arango-single-request-max-frontier"},
                "In Arango auto mode, use one request per edge scan up to this source-collection frontier size",
                std::string{"50000"});

        program.add_value_option("streaming-candidate-batch-size", {"--streaming-candidate-batch-size"},
                                 "Candidate IDs emitted from backends to no-predecessor BFS per transient batch",
                                 std::string{"16384"});

        program.add_value_option("redis-pipeline-batch-size", {"--redis-pipeline-batch-size"},
                                 "Maximum Redis GET keys per pipeline in no-predecessor streaming BFS",
                                 std::string{"20000"});

        program.add_value_option("backend-scheduler-priority", {"--backend-scheduler-priority"},
                                 "Per-BFS remote-backend scheduling priority (-100..100; higher runs first)",
                                 std::string{"0"});

        program.add_value_option("backend-scheduler-weight", {"--backend-scheduler-weight"},
                                 "Per-BFS relative share for weighted-fair backend scheduling (1..64)",
                                 std::string{"1"});

        program.add_value_option("backend-request-retries", {"--backend-request-retries"},
                                 "Retries after a failed backend neighbor request", std::string{"2"});

        program.add_value_option("backend-retry-delay-ms", {"--backend-retry-delay-ms"},
                                 "Initial backend retry delay in milliseconds", std::string{"100"});

        program.add_value_option("thread-count", {"--thread-count"}, "Worker count reserved for the BFS thread pool",
                                 default_thread_count());
    }

    GraphRuntimeConfig parse_runtime_config(const CommandLine &args) {
        GraphRuntimeConfig result;
        if (const auto domain = configured_domain(args)) {
            result.logical_graph_schema_path = domain->logical_graph_schema_path.string();
            result.data_path = domain->data_path.string();
            result.backend_config_path = domain->backend_config_path.string();
            if (domain->native_id_map_path) {
                result.native_id_map_path = domain->native_id_map_path->string();
            }
            if (domain->cache_plan_path) {
                result.cache_plan_path = domain->cache_plan_path->string();
            }
            result.graph_version = domain->runtime.graph_version;
            result.verbose = domain->runtime.verbose;
        }

        const auto &schema_override = args.string_value("logical-graph-schema");
        const auto &data_override = args.string_value("data");
        const auto &backend_override = args.string_value("backend-config");
        const auto &native_id_map_override = args.string_value("native-id-map");
        const auto &cache_plan_override = args.string_value("cache-plan");
        if (!schema_override.empty()) {
            result.logical_graph_schema_path = schema_override;
        }
        if (!data_override.empty()) {
            result.data_path = data_override;
        }
        if (!backend_override.empty()) {
            result.backend_config_path = backend_override;
        }
        if (!native_id_map_override.empty()) {
            result.native_id_map_path = native_id_map_override;
        }
        if (!cache_plan_override.empty()) {
            result.cache_plan_path = cache_plan_override;
        }
        result.verbose = result.verbose || args.bool_value("verbose");

        if (result.logical_graph_schema_path.empty() || result.data_path.empty()) {
            throw std::runtime_error(
                    "Provide --domain, or provide both --logical-graph-schema and --data compatibility overrides");
        }
        return result;
    }

    NodeRef parse_source(const CommandLine &args, const DatabaseContext &context) {
        std::string source = args.string_value("start-uuid");
        if (source.empty()) {
            const auto domain = configured_domain(args);
            if (domain && domain->bfs.start_uuid) {
                source = *domain->bfs.start_uuid;
            }
        }
        if (source.empty()) {
            throw std::runtime_error("Provide --start-uuid or domain.defaults.bfs.startUuid");
        }
        return context.node_ref(global_id_from_string(source));
    }

    ExpandSpec parse_expand_spec(const CommandLine &args, const domain::LogicalGraphSchema &schema) {
        std::string direction = args.string_value("direction");
        if (direction.empty()) {
            const auto domain = configured_domain(args);
            direction = domain && domain->bfs.direction ? *domain->bfs.direction : "undirected";
        }
        return ExpandSpec{.allowed_edge_types = parse_edge_type_filter(args.string_value("allowed-edge-types"), schema),
                          .allowed_target_node_types =
                                  parse_node_type_filter(args.string_value("allowed-target-node-types"), schema),
                          .direction = parse_direction(direction)};
    }

    BfsOptions parse_bfs_options(const CommandLine &args) {
        std::string max_depth_value = args.string_value("max-depth");
        if (max_depth_value.empty()) {
            const auto domain = configured_domain(args);
            max_depth_value = domain && domain->bfs.max_depth ? std::to_string(*domain->bfs.max_depth) : "7";
        }
        const int max_depth = parse_integer(max_depth_value, "--max-depth");
        const int neighbor_batch_size = args.int_value("neighbor-batch-size");
        const int neo4j_max_concurrent_requests = args.int_value("neo4j-max-concurrent-requests");
        const int arango_max_concurrent_requests = args.int_value("arango-max-concurrent-requests");
        const auto arango_request_strategy =
                parse_arango_request_strategy(args.string_value("arango-request-strategy"));
        const int arango_single_request_max_frontier = args.int_value("arango-single-request-max-frontier");
        const int streaming_candidate_batch_size = args.int_value("streaming-candidate-batch-size");
        const int redis_pipeline_batch_size = args.int_value("redis-pipeline-batch-size");
        const int backend_scheduler_priority = args.int_value("backend-scheduler-priority");
        const int backend_scheduler_weight = args.int_value("backend-scheduler-weight");
        const int backend_request_retries = args.int_value("backend-request-retries");
        const int backend_retry_delay_ms = args.int_value("backend-retry-delay-ms");
        const int thread_count = args.int_value("thread-count");

        if (neighbor_batch_size <= 0) {
            throw std::runtime_error("--neighbor-batch-size must be greater than zero");
        }
        if (neo4j_max_concurrent_requests <= 0) {
            throw std::runtime_error("--neo4j-max-concurrent-requests must be greater than zero");
        }
        if (arango_max_concurrent_requests <= 0) {
            throw std::runtime_error("--arango-max-concurrent-requests must be greater than zero");
        }
        if (arango_single_request_max_frontier <= 0) {
            throw std::runtime_error("--arango-single-request-max-frontier must be greater than zero");
        }
        if (streaming_candidate_batch_size <= 0) {
            throw std::runtime_error("--streaming-candidate-batch-size must be greater than zero");
        }
        if (redis_pipeline_batch_size <= 0) {
            throw std::runtime_error("--redis-pipeline-batch-size must be greater than zero");
        }
        if (backend_scheduler_priority < -100 || backend_scheduler_priority > 100) {
            throw std::runtime_error("--backend-scheduler-priority must be between -100 and 100");
        }
        if (backend_scheduler_weight <= 0 || backend_scheduler_weight > 64) {
            throw std::runtime_error("--backend-scheduler-weight must be between 1 and 64");
        }
        if (backend_request_retries < 0) {
            throw std::runtime_error("--backend-request-retries must be zero or greater");
        }
        if (backend_retry_delay_ms < 0) {
            throw std::runtime_error("--backend-retry-delay-ms must be zero or greater");
        }
        if (thread_count <= 0) {
            throw std::runtime_error("--thread-count must be greater than zero");
        }

        BfsOptions options{.max_depth = std::nullopt,
                           .return_predecessors = !args.bool_value("no-predecessors"),
                           .allow_native_pushdown = args.bool_value("allow-native-pushdown"),
                           .allow_partial_acceleration = !args.bool_value("no-partial-acceleration"),
                           .neighbor_batch_size = static_cast<std::size_t>(neighbor_batch_size),
                           .neo4j_max_concurrent_requests = static_cast<std::size_t>(neo4j_max_concurrent_requests),
                           .arango_max_concurrent_requests = static_cast<std::size_t>(arango_max_concurrent_requests),
                           .arango_request_strategy = arango_request_strategy,
                           .arango_single_request_max_frontier =
                                   static_cast<std::size_t>(arango_single_request_max_frontier),
                           .streaming_candidate_batch_size = static_cast<std::size_t>(streaming_candidate_batch_size),
                           .redis_pipeline_batch_size = static_cast<std::size_t>(redis_pipeline_batch_size),
                           .backend_scheduler_priority = backend_scheduler_priority,
                           .backend_scheduler_weight = static_cast<std::size_t>(backend_scheduler_weight),
                           .backend_request_retries = static_cast<std::size_t>(backend_request_retries),
                           .backend_retry_delay_ms = static_cast<std::size_t>(backend_retry_delay_ms),
                           .thread_count = static_cast<std::size_t>(thread_count)};

        if (max_depth >= 0) {
            options.max_depth = static_cast<std::uint32_t>(max_depth);
        }

        return options;
    }

    void write_bfs_distances_csv(std::ostream &output, const BfsResult &result) {
        output << "vertex_id;distance\n";

        std::vector<std::pair<GlobalId, std::uint32_t>> ordered(result.distance.begin(), result.distance.end());
        std::ranges::sort(ordered, [](const auto &left, const auto &right) {
            return left.second != right.second ? left.second < right.second
                                               : global_id_to_string(left.first) < global_id_to_string(right.first);
        });

        for (const auto &[vertex_id, distance]: ordered) {
            output << vertex_id << ';' << distance << '\n';
        }
    }

} // namespace polygraph::cli
