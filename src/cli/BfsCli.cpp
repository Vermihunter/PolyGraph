/**
 * @file src/cli/BfsCli.cpp
 * @brief Implementation for bfs cli in the command-line and runtime configuration module.
 */

//
// Created by akos on 7. 7. 2026.
//

#include "cli/BfsCli.hpp"
#include "utilities/uuid.hpp"

#include "logging/LoggingSetup.hpp"
#include "utilities/logging.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace cpgdb::cli {
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
        program.add_value_option("logical-graph-schema", {"-lgs", "--logical-graph-schema"},
                                 "Path to the logical graph schema JSON file", std::nullopt, true);

        program.add_value_option("data", {"--data"}, "Path to the JSON data file used for host/GPU projection input",
                                 std::nullopt, true);

        program.add_value_option("backend-config", {"--backend-config"},
                                 "Path to JSON config describing enabled backends and connections", std::string{});

        program.add_flag_option("verbose", {"-v", "--verbose"}, "Enable verbose output");
    }

    void add_bfs_arguments(CommandLine &program) {
        program.add_value_option("start-uuid", {"--start-uuid"}, "Unsigned 64-bit global ID of the BFS source vertex",
                                 std::nullopt, true);


        program.add_value_option("max-depth", {"--max-depth"}, "BFS max depth; use -1 for unlimited", std::string{"7"});

        program.add_value_option("direction", {"--direction"}, "BFS direction: directed, undirected, or both",
                                 std::string{"undirected"});

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

        program.add_value_option("backend-request-retries", {"--backend-request-retries"},
                                 "Retries after a failed backend neighbor request", std::string{"2"});

        program.add_value_option("backend-retry-delay-ms", {"--backend-retry-delay-ms"},
                                 "Initial backend retry delay in milliseconds", std::string{"100"});

        program.add_value_option("thread-count", {"--thread-count"}, "Worker count reserved for the BFS thread pool",
                                 default_thread_count());
    }

    GraphRuntimeConfig parse_runtime_config(const CommandLine &args) {
        std::optional<std::string> backend_config_path;
        const auto &backend_config_value = args.string_value("backend-config");
        if (!backend_config_value.empty()) {
            backend_config_path = backend_config_value;
        }

        return GraphRuntimeConfig{.logical_graph_schema_path = args.string_value("logical-graph-schema"),
                                  .data_path = args.string_value("data"),
                                  .backend_config_path = backend_config_path,
                                  .graph_version = 1,
                                  .verbose = args.bool_value("verbose")};
    }

    NodeRef parse_source(const CommandLine &args, const DatabaseContext &context) {
        const auto id = global_id_from_string(args.string_value("start-uuid"));
        return context.node_ref(id);
    }

    ExpandSpec parse_expand_spec(const CommandLine &args, const domain::LogicalGraphSchema &schema) {
        return ExpandSpec{.allowed_edge_types = parse_edge_type_filter(args.string_value("allowed-edge-types"), schema),
                          .allowed_target_node_types =
                                  parse_node_type_filter(args.string_value("allowed-target-node-types"), schema),
                          .direction = parse_direction(args.string_value("direction"))};
    }

    BfsOptions parse_bfs_options(const CommandLine &args) {
        const int max_depth = args.int_value("max-depth");
        const int neighbor_batch_size = args.int_value("neighbor-batch-size");
        const int neo4j_max_concurrent_requests = args.int_value("neo4j-max-concurrent-requests");
        const int arango_max_concurrent_requests = args.int_value("arango-max-concurrent-requests");
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

} // namespace cpgdb::cli
