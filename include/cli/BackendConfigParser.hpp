#ifndef CPGDB_BACKEND_CONFIG_PARSER_HPP
#define CPGDB_BACKEND_CONFIG_PARSER_HPP



#include <cstdint>
#include <fstream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "domain/graph/LogicalGraphSchema.hpp"
#include "domain/graph/TypeId.hpp"
#include "infrastructure/BackendCapabilities.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "infrastructure/BackendId.hpp"
#include "infrastructure/BackendKind.hpp"
#include "infrastructure/DatabaseCredentials.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "infrastructure/GraphCoverage.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/json.hpp"

namespace cpgdb::config {

inline std::string backend_id_to_string(BackendId id) {
    switch (id) {
        case GPU: return "GPU";
        case MEMGRAPH: return "Memgraph";
        case NEO4J: return "Neo4J";
        case ARANGO: return "ArangoDB";
        default: return "Backend#" + std::to_string(id);
    }
}

inline BackendId backend_id_from_string(const std::string& value) {
    static const std::unordered_map<std::string, BackendId> map{
        {"GPU", GPU},
        {"CuGraph", GPU},
        {"cuGraph", GPU},
        {"Memgraph", MEMGRAPH},
        {"MEMGRAPH", MEMGRAPH},
        {"Neo4J", NEO4J},
        {"Neo4j", NEO4J},
        {"NEO4J", NEO4J},
        {"ArangoDB", ARANGO},
        {"Arango", ARANGO},
        {"ARANGO", ARANGO},
    };

    if (auto it = map.find(value); it != map.end()) {
        return it->second;
    }

    throw std::runtime_error("Unknown backend id: " + value);
}

inline BackendKind backend_kind_from_string(const std::string& value) {
    static const std::unordered_map<std::string, BackendKind> map{
        {"Neo4j", BackendKind::Neo4j},
        {"Neo4J", BackendKind::Neo4j},
        {"Arango", BackendKind::Arango},
        {"ArangoDB", BackendKind::Arango},
        {"Memgraph", BackendKind::Memgraph},
        {"RamCache", BackendKind::RamCache},
        {"CuGraph", BackendKind::CuGraph},
        {"GPU", BackendKind::CuGraph},
        {"Janus", BackendKind::Janus},
    };

    if (auto it = map.find(value); it != map.end()) {
        return it->second;
    }

    throw std::runtime_error("Unknown backend kind: " + value);
}

inline std::string backend_kind_to_string(BackendKind kind) {
    switch (kind) {
        case BackendKind::Neo4j: return "Neo4j";
        case BackendKind::Arango: return "Arango";
        case BackendKind::Memgraph: return "Memgraph";
        case BackendKind::RamCache: return "RamCache";
        case BackendKind::CuGraph: return "CuGraph";
        case BackendKind::Janus: return "Janus";
        case BackendKind::COUNT: break;
    }
    return "Unknown";
}

inline json load_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Could not open backend config: " + path);
    }
    return json::parse(in);
}

inline std::unordered_set<TypeId>
node_type_set_from_names(const json& names,
                         const domain::LogicalGraphSchema& schema) {
    std::unordered_set<TypeId> result;
    if (names.is_null()) {
        return result;
    }
    if (!names.is_array()) {
        throw std::runtime_error("coverage.node_types must be an array");
    }
    for (const auto& name : names) {
        result.insert(schema.node_type_id(name.get<std::string>()));
    }
    return result;
}

inline std::unordered_set<TypeId>
edge_type_set_from_names(const json& names,
                         const domain::LogicalGraphSchema& schema) {
    std::unordered_set<TypeId> result;
    if (names.is_null()) {
        return result;
    }
    if (!names.is_array()) {
        throw std::runtime_error("coverage.edge_types must be an array");
    }
    for (const auto& name : names) {
        result.insert(schema.edge_type_id(name.get<std::string>()));
    }
    return result;
}

inline GraphCoverage parse_coverage(
    const json& backend_json,
    BackendId id,
    const domain::LogicalGraphSchema& schema,
    const std::unordered_map<BackendId, std::unordered_set<TypeId>>& nodes_by_backend,
    const std::unordered_map<BackendId, std::unordered_set<TypeId>>& edges_by_backend,
    std::uint64_t graph_version
) {
    const json coverage_json = backend_json.value("coverage", json::object());
    const std::string mode = coverage_json.value("mode", "schema-source");

    GraphCoverage coverage{
        .fresh = coverage_json.value("fresh", true),
        .covers_all_node_types = false,
        .covers_all_edge_types = false,
        .complete_for_requested_subgraph = coverage_json.value("complete_for_requested_subgraph", false),
        .node_types = {},
        .edge_types = {},
        .graph_version = coverage_json.value("graph_version", graph_version)
    };

    if (mode == "schema-source" || mode == "from-schema") {
        if (auto it = nodes_by_backend.find(id); it != nodes_by_backend.end()) {
            coverage.node_types = it->second;
        }
        if (auto it = edges_by_backend.find(id); it != edges_by_backend.end()) {
            coverage.edge_types = it->second;
        }
    } else if (mode == "all") {
        coverage.covers_all_node_types = true;
        coverage.covers_all_edge_types = true;
    } else if (mode == "explicit") {
        coverage.covers_all_node_types = coverage_json.value("covers_all_node_types", false);
        coverage.covers_all_edge_types = coverage_json.value("covers_all_edge_types", false);
        coverage.node_types = node_type_set_from_names(coverage_json.value("node_types", json::array()), schema);
        coverage.edge_types = edge_type_set_from_names(coverage_json.value("edge_types", json::array()), schema);
    } else if (mode == "empty" || mode == "none") {
        // Keep empty coverage.
    } else {
        throw std::runtime_error("Unknown coverage mode for backend " + backend_id_to_string(id) + ": " + mode);
    }

    return coverage;
}

inline BackendCapabilities parse_capabilities(const json& backend_json) {
    const json caps = backend_json.value("capabilities", json::object());

    return BackendCapabilities{
        .supports_batch_neighbors = caps.value("batch_neighbors", caps.value("supports_batch_neighbors", false)),
        .supports_native_bfs = caps.value("native_bfs", caps.value("supports_native_bfs", false)),
        .supports_native_property_filters = caps.value("property_filters", caps.value("supports_native_property_filters", false)),
        .supports_gpu_execution = caps.value("gpu_execution", caps.value("supports_gpu_execution", false)),
        .supports_replica_parallelism = caps.value("replica_parallelism", caps.value("supports_replica_parallelism", false)),
    };
}

inline BackendConnectionConfig parse_connection_config(const json& backend_json,
                                                       BackendId id,
                                                       BackendKind kind) {
    const json conn = backend_json.value("connection", json::object());

    BackendConnectionConfig config{
        .id = id,
        .kind = kind,
        .name = backend_json.value("name", backend_id_to_string(id)),
        .endpoint = conn.value("endpoint", backend_json.value("endpoint", std::string{})),
        .database_name = conn.value("database", conn.value("database_name", backend_json.value("database_name", std::string{}))),
        .credentials = std::nullopt,
        .replica_endpoints = {}
    };

    if (conn.contains("credentials") && !conn["credentials"].is_null()) {
        const json credentials = conn["credentials"];
        config.credentials = DatabaseCredentials{
            .username = credentials.value("username", std::string{}),
            .password = credentials.value("password", std::string{})
        };
    } else if (backend_json.contains("credentials") && !backend_json["credentials"].is_null()) {
        const json credentials = backend_json["credentials"];
        config.credentials = DatabaseCredentials{
            .username = credentials.value("username", std::string{}),
            .password = credentials.value("password", std::string{})
        };
    }

    const json replicas = conn.value("replicas", conn.value("replica_endpoints", json::array()));
    if (replicas.is_array()) {
        for (const auto& endpoint : replicas) {
            config.replica_endpoints.push_back(endpoint.get<std::string>());
        }
    }

    return config;
}

inline BackendDescriptor parse_descriptor(
    const json& backend_json,
    BackendId id,
    BackendKind kind,
    const domain::LogicalGraphSchema& schema,
    const std::unordered_map<BackendId, std::unordered_set<TypeId>>& nodes_by_backend,
    const std::unordered_map<BackendId, std::unordered_set<TypeId>>& edges_by_backend,
    std::uint64_t graph_version
) {
    return BackendDescriptor{
        .id = id,
        .kind = kind,
        .capabilities = parse_capabilities(backend_json),
        .coverage = parse_coverage(backend_json, id, schema, nodes_by_backend, edges_by_backend, graph_version),
        .priority = backend_json.value("priority", 100),
        .name = backend_json.value("name", backend_id_to_string(id)),
    };
}


inline bool backend_json_enabled(const json& backend_json) {
    return backend_json.value("enabled", true);
}

inline json default_backend_config_json() {
    return {
        {"version", 1},
        {"backends", json::array({
            {
                {"id", "Neo4J"},
                {"kind", "Neo4j"},
                {"enabled", true},
                {"name", "Neo4j Primary"},
                {"priority", 50},
                {"connection", {
                    {"endpoint", "http://localhost:7474/db/neo4j/tx/commit"},
                    {"database", "neo4j"},
                    {"credentials", {{"username", "neo4j"}, {"password", "password"}}}
                }},
                {"capabilities", {{"batch_neighbors", true}, {"native_bfs", false}, {"property_filters", true}}},
                {"coverage", {{"mode", "schema-source"}, {"fresh", true}, {"complete_for_requested_subgraph", false}}}
            },
            {
                {"id", "ArangoDB"},
                {"kind", "ArangoDB"},
                {"enabled", true},
                {"name", "ArangoDB Primary"},
                {"priority", 0},
                {"connection", {
                    {"endpoint", "http://localhost:8529/_db/university/_api/cursor"},
                    {"database", "university"},
                    {"credentials", {{"username", "root"}, {"password", "rootpassword"}}}
                }},
                {"capabilities", {{"batch_neighbors", true}, {"native_bfs", false}, {"gpu_execution", false}}},
                {"coverage", {{"mode", "schema-source"}, {"fresh", true}, {"complete_for_requested_subgraph", false}}}
            },
            {
                {"id", "GPU"},
                {"kind", "CuGraph"},
                {"enabled", true},
                {"name", "GPU acceleration"},
                {"priority", 1},
                {"connection", {{"endpoint", ""}, {"database", ""}}},
                {"capabilities", {{"batch_neighbors", true}, {"native_bfs", false}, {"property_filters", false}, {"gpu_execution", true}}},
                {"coverage", {{"mode", "schema-source"}, {"fresh", true}, {"complete_for_requested_subgraph", false}}}
            }
        })}
    };
}

inline json load_backend_config_file_or_default(const std::optional<std::string>& path) {
    if (path.has_value() && !path->empty()) {
        return load_json_file(*path);
    }

    return default_backend_config_json();
}

inline const json& backend_array_or_throw(const json& root) {
    if (!root.contains("backends") || !root["backends"].is_array()) {
        throw std::runtime_error("backend config field 'backends' must be an array");
    }

    return root["backends"];
}


inline bool backend_enabled(const DatabaseContext& ctx, BackendId id) {
    return ctx.backend_registry->contains(id);
}

inline void print_backend_summary(const DatabaseContext& ctx, std::ostream& out) {
    out << "Graph DB backend config\n";
    out << "  enabled backends:";
    for (const auto& [id, descriptor] : ctx.backend_registry->descriptors()) {
        out << ' ' << backend_id_to_string(id);
    }
    out << "\n";

    out << "  backends:\n";
    for (const auto& [id, descriptor] : ctx.backend_registry->descriptors()) {
        out << "    - " << backend_id_to_string(id)
            << " kind=" << backend_kind_to_string(descriptor.kind)
            << " priority=" << descriptor.priority
            << " nodes=" << descriptor.coverage.node_types.size()
            << " edges=" << descriptor.coverage.edge_types.size()
            << "\n";
    }
}

} // namespace cpgdb::config

#endif // CPGDB_BACKEND_CONFIG_PARSER_HPP
