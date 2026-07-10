//
// Created by akos on 7. 7. 2026.
//

#include "cli/BfsCli.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid.hpp>

#include "logging/LoggingSetup.hpp"

namespace cpgdb::cli {
namespace {

[[nodiscard]] std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> result;
    std::string current;
    std::stringstream stream(value);

    while (std::getline(stream, current, ',')) {
        current.erase(current.begin(), std::find_if(current.begin(), current.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        current.erase(std::find_if(current.rbegin(), current.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), current.end());

        if (!current.empty()) {
            result.push_back(current);
        }
    }

    return result;
}

[[nodiscard]] Direction parse_direction(const std::string& value) {
    if (value == "directed" || value == "Directed") {
        return Direction::Directed;
    }
    if (value == "undirected" || value == "Undirected") {
        return Direction::Undirected;
    }
    if (value == "both" || value == "Both") {
        return Direction::Both;
    }

    throw std::runtime_error(
        "Unknown --direction value: " + value + " (expected directed, undirected, both)"
    );
}

[[nodiscard]] std::vector<TypeId> parse_edge_type_filter(
    const std::string& csv,
    const domain::LogicalGraphSchema& schema
) {
    std::vector<TypeId> result;
    for (const auto& name : split_csv(csv)) {
        result.push_back(schema.edge_type_id(name));
    }
    return result;
}

[[nodiscard]] std::vector<TypeId> parse_node_type_filter(
    const std::string& csv,
    const domain::LogicalGraphSchema& schema
) {
    std::vector<TypeId> result;
    for (const auto& name : split_csv(csv)) {
        result.push_back(schema.node_type_id(name));
    }
    return result;
}

} // namespace

void add_runtime_arguments(argparse::ArgumentParser& program) {
    program.add_argument("-lgs", "--logical-graph-schema")
        .help("Path to the logical graph schema JSON file")
        .required();

    program.add_argument("--data")
        .help("Path to the JSON data file used for host/GPU projection input")
        .required();

    program.add_argument("--backend-config")
        .help("Path to JSON config describing enabled backends and connections")
        .default_value(std::string{});

    program.add_argument("--log-dir")
        .default_value(std::string{"logs"})
        .help("Directory for event logs.");

    program.add_argument("--log-name")
        .default_value(std::string{})
        .help("Log filename without extension. If omitted, current date/time is used.");

    program.add_argument("--log-file")
        .default_value(std::string{})
        .help("Exact log file path. Overrides --log-dir and --log-name.");

    program.add_argument("--no-event-log")
        .default_value(false)
        .implicit_value(true)
        .help("Disable structured event logging.");

    program.add_argument("-v", "--verbose")
        .help("Enable verbose output")
        .default_value(false)
        .implicit_value(true);
}

void add_bfs_arguments(argparse::ArgumentParser& program) {
    program.add_argument("--start-uuid")
        .help("UUID of the BFS source vertex")
        .default_value(std::string{"11a96cb7-c043-4aa7-aa35-2297e9be7f06"});

    program.add_argument("--start-type")
        .help("Logical node type of the BFS source vertex")
        .default_value(std::string{"Faculty"});

    program.add_argument("--max-depth")
        .help("BFS max depth. Use -1 for unlimited.")
        .default_value(7)
        .scan<'i', int>();

    program.add_argument("--direction")
        .help("BFS direction: directed, undirected, or both")
        .default_value(std::string{"undirected"});

    program.add_argument("--allowed-edge-types")
        .help("Comma-separated logical edge type filter. Empty means all.")
        .default_value(std::string{});

    program.add_argument("--allowed-target-node-types")
        .help("Comma-separated logical target node type filter. Empty means all.")
        .default_value(std::string{});

    program.add_argument("--allow-native-pushdown")
        .help("Allow native backend BFS pushdown when possible")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--no-partial-acceleration")
        .help("Disable partial acceleration/projection usage")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--no-predecessors")
        .help("Do not request predecessor information")
        .default_value(false)
        .implicit_value(true);
}

GraphRuntimeConfig parse_runtime_config(const argparse::ArgumentParser& args) {
    std::optional<std::string> backend_config_path;
    const auto backend_config_value = args.get<std::string>("--backend-config");
    if (!backend_config_value.empty()) {
        backend_config_path = backend_config_value;
    }

    return GraphRuntimeConfig{
        .logical_graph_schema_path = args.get<std::string>("--logical-graph-schema"),
        .data_path = args.get<std::string>("--data"),
        .backend_config_path = backend_config_path,
        .graph_version = 1,
        .verbose = args.get<bool>("--verbose")
    };
}

void parse_log_options(const argparse::ArgumentParser& args) {
    cpgdb::logging::LoggingOptions log_options;

    log_options.enabled = !args.get<bool>("--no-event-log");
    log_options.log_dir = args.get<std::string>("--log-dir");
    log_options.log_name = args.get<std::string>("--log-name");

    const auto explicit_log_file = args.get<std::string>("--log-file");
    if (!explicit_log_file.empty()) {
        log_options.log_file = explicit_log_file;
    }

    logger = cpgdb::logging::create_event_logger(log_options);
}

NodeRef parse_source(const argparse::ArgumentParser& args,
                     const domain::LogicalGraphSchema& schema) {
    return NodeRef{
        .global_id = boost::lexical_cast<boost::uuids::uuid>(args.get<std::string>("--start-uuid")),
        .type = schema.node_type_id(args.get<std::string>("--start-type"))
    };
}

ExpandSpec parse_expand_spec(const argparse::ArgumentParser& args,
                             const domain::LogicalGraphSchema& schema) {
    return ExpandSpec{
        .allowed_edge_types = parse_edge_type_filter(args.get<std::string>("--allowed-edge-types"), schema),
        .allowed_target_node_types = parse_node_type_filter(args.get<std::string>("--allowed-target-node-types"), schema),
        .direction = parse_direction(args.get<std::string>("--direction"))
    };
}

BfsOptions parse_bfs_options(const argparse::ArgumentParser& args) {
    const int max_depth = args.get<int>("--max-depth");

    BfsOptions options{
        .max_depth = std::nullopt,
        .return_predecessors = !args.get<bool>("--no-predecessors"),
        .allow_native_pushdown = args.get<bool>("--allow-native-pushdown"),
        .allow_partial_acceleration = !args.get<bool>("--no-partial-acceleration")
    };

    if (max_depth >= 0) {
        options.max_depth = static_cast<std::uint32_t>(max_depth);
    }

    return options;
}

} // namespace cpgdb::cli
