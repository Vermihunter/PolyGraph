/**
 * @file src/cli/BfsCli.cpp
 * @brief Implementation for bfs cli in the command-line and runtime configuration module.
 */

//
// Created by akos on 7. 7. 2026.
//

#include "cli/BfsCli.hpp"

#include "logging/LoggingSetup.hpp"
#include "utilities/logging.hpp"

#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cctype>
#include <sstream>
#include <stdexcept>

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
        program.add_value_option("start-uuid", {"--start-uuid"}, "UUID of the BFS source vertex",
                                 std::string{"11a96cb7-c043-4aa7-aa35-2297e9be7f06"});

        program.add_value_option("start-type", {"--start-type"}, "Logical node type of the BFS source vertex",
                                 std::string{"Faculty"});

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

    NodeRef parse_source(const CommandLine &args, const domain::LogicalGraphSchema &schema) {
        return NodeRef{.global_id = boost::lexical_cast<boost::uuids::uuid>(args.string_value("start-uuid")),
                       .type = schema.node_type_id(args.string_value("start-type"))};
    }

    ExpandSpec parse_expand_spec(const CommandLine &args, const domain::LogicalGraphSchema &schema) {
        return ExpandSpec{.allowed_edge_types = parse_edge_type_filter(args.string_value("allowed-edge-types"), schema),
                          .allowed_target_node_types =
                                  parse_node_type_filter(args.string_value("allowed-target-node-types"), schema),
                          .direction = parse_direction(args.string_value("direction"))};
    }

    BfsOptions parse_bfs_options(const CommandLine &args) {
        const int max_depth = args.int_value("max-depth");

        BfsOptions options{.max_depth = std::nullopt,
                           .return_predecessors = !args.bool_value("no-predecessors"),
                           .allow_native_pushdown = args.bool_value("allow-native-pushdown"),
                           .allow_partial_acceleration = !args.bool_value("no-partial-acceleration")};

        if (max_depth >= 0) {
            options.max_depth = static_cast<std::uint32_t>(max_depth);
        }

        return options;
    }

    void write_bfs_distances_csv(std::ostream &output, const BfsResult &result) {
        output << "vertex_id;distance\n";

        std::vector<std::pair<GlobalId, std::uint32_t>> ordered(result.distance.begin(), result.distance.end());
        std::ranges::sort(ordered, [](const auto &left, const auto &right) {
            return left.second != right.second
                           ? left.second < right.second
                           : boost::uuids::to_string(left.first) < boost::uuids::to_string(right.first);
        });

        for (const auto &[vertex_id, distance]: ordered) {
            output << vertex_id << ';' << distance << '\n';
        }
    }

} // namespace cpgdb::cli
