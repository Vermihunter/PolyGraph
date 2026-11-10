#include "ingestion/GraphManifest.hpp"
#include "Internal.hpp"
#include "utilities/GraphWorkspace.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <unistd.h>

namespace polygraph::ingestion {
    namespace {
        [[noreturn]] void fail(const std::string &field, const std::string &message) {
            throw Error("graph." + field + ": " + message);
        }
        std::string text(const Json &object, const std::string &key, const std::string &field) {
            if (!object.is_object() || !object.contains(key) || !object[key].is_string() ||
                object[key].get_ref<const std::string &>().empty() ||
                object[key].get_ref<const std::string &>().find('\0') != std::string::npos)
                fail(field, key + " must be a nonempty string without NUL");
            return object[key].get<std::string>();
        }
        void file(const fs::path &path, const std::string &field) {
            std::error_code error;
            if (!fs::is_regular_file(path, error) || error || ::access(path.c_str(), R_OK) != 0)
                fail(field, "required readable file is unavailable: " + path.string());
        }
        Json json_file(const fs::path &path, const std::string &field) {
            file(path, field);
            try {
                return read_json(path);
            } catch (const std::exception &) {
                fail(field, "invalid JSON file: " + path.string());
            }
        }
        void keys(const Json &object, const std::set<std::string> &allowed, const std::string &field) {
            if (!object.is_object())
                fail(field, "must be an object");
            for (const auto &[key, value]: object.items()) {
                (void) value;
                if (!allowed.contains(key))
                    fail(field, "unknown field: " + key);
            }
        }
        unsigned number(const Json &options, const std::string &key, unsigned fallback,
                        const std::string &field = "options") {
            if (!options.contains(key))
                return fallback;
            const auto &value = options[key];
            if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<unsigned>::max())
                fail(field, key + " must be an unsigned 32-bit integer");
            return value.get<unsigned>();
        }
        std::string strategy(const InputAdapterDefinition &adapter) { return input_id_strategy(adapter.id_policy); }
        void writable_output(const fs::path &output) {
            auto ancestor = output;
            std::error_code error;
            while (!fs::exists(ancestor, error)) {
                if (error)
                    fail("output", "cannot inspect output ancestor: " + ancestor.string());
                const auto parent = ancestor.parent_path();
                if (parent.empty() || parent == ancestor)
                    fail("output", "no existing output ancestor: " + output.string());
                ancestor = parent;
            }
            constexpr auto writes = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
            if (!fs::is_directory(ancestor) || (fs::status(ancestor).permissions() & writes) == fs::perms::none ||
                ::access(ancestor.c_str(), W_OK | X_OK) != 0)
                fail("output", "output ancestor is not a writable/searchable directory: " + ancestor.string());
        }
        struct Schema {
            std::set<std::string> nodes;
            std::set<std::string> edges;
        };
        Schema check_schema(const fs::path &path) {
            const auto schema = json_file(path, "schema");
            if (!schema.is_object() || !schema.contains("nodeTypes") || !schema["nodeTypes"].is_array() ||
                !schema.contains("relations") || !schema["relations"].is_array())
                fail("schema", "nodeTypes and relations arrays are required");
            Schema result;
            std::map<std::string, std::string> sanitized;
            const auto entries = [&](const Json &values, std::set<std::string> &names) {
                for (const auto &value: values) {
                    const auto name = text(value, "name", "schema");
                    if (!names.insert(name).second)
                        fail("schema", "duplicate type name: " + name);
                    const auto safe = sanitize_name(name);
                    if (safe == "Proxy" || safe == "polygraph_bulk_import_lock" || safe.size() > 200)
                        fail("schema", "reserved or oversized type name: " + name);
                    if (const auto [it, inserted] = sanitized.emplace(safe, name); !inserted && it->second != name)
                        fail("schema", "type names collide after sanitization: " + name);
                    const auto properties = value.value("properties", Json::array());
                    if (!properties.is_array())
                        fail("schema", "properties must be an array on " + name);
                    std::set<std::string> attributes;
                    for (const auto &property: properties) {
                        const auto key = text(property, "name", "schema");
                        if (!attributes.insert(key).second)
                            fail("schema", "duplicate property on " + name + ": " + key);
                        const auto type = text(property, "type", "schema");
                        static const std::set<std::string> types{"string", "integer", "boolean",
                                                                 "float",  "date",    "datetime"};
                        if (!types.contains(type))
                            fail("schema", "unsupported property type on " + name + ": " + type);
                    }
                }
            };
            entries(schema["nodeTypes"], result.nodes);
            entries(schema["relations"], result.edges);
            for (const auto &edge: schema["relations"]) {
                if (!result.nodes.contains(text(edge, "from", "schema")) ||
                    !result.nodes.contains(text(edge, "to", "schema")))
                    fail("schema", "relationship endpoint type is not declared: " + edge["name"].get<std::string>());
                if (edge.contains("directed") && !edge["directed"].is_boolean())
                    fail("schema", "directed must be boolean");
            }
            return result;
        }
        void command(const Json &target, const std::string &key, bool required = false) {
            if (!target.contains(key)) {
                if (required)
                    fail("execution", "Neo4J requires " + key);
                return;
            }
            const auto &argv = target[key];
            if (!argv.is_array() || argv.empty())
                fail("execution", key + " must be a nonempty argv array");
            for (const auto &argument: argv)
                if (!argument.is_string() || argument.get_ref<const std::string &>().find('\0') != std::string::npos)
                    fail("execution", key + " contains an invalid argument");
            const auto executable = argv[0].get<std::string>();
            const auto usable = [](const fs::path &candidate) {
                return fs::is_regular_file(candidate) && ::access(candidate.c_str(), X_OK) == 0;
            };
            if (executable.find('/') != std::string::npos) {
                if (usable(executable))
                    return;
            } else if (const auto *path = std::getenv("PATH")) {
                std::istringstream parts(path);
                std::string part;
                while (std::getline(parts, part, ':'))
                    if (usable(fs::path(part.empty() ? "." : part) / executable))
                        return;
            }
            fail("execution", key + " executable is unavailable: " + executable);
        }
        void execution_prerequisites(const GraphManifest &graph, const Placement &placement) {
            if (graph.target_config.empty())
                fail("execution", "paths.targetConfig is required for --execution");
            Json targets;
            try {
                targets = detail::checked_targets(json_file(graph.target_config, "execution"), placement.backends);
            } catch (const std::exception &error) {
                fail("execution", error.what());
            }
            const auto capabilities = native_executor_capabilities();
            for (const auto backend: placement.backends) {
                const auto name = backend_name(backend);
                if (!capabilities[name].get<bool>())
                    fail("execution", name + " native executor is unavailable in this build");
                const auto &target = targets[name];
                if (backend == Backend::arango) {
                    try {
                        detail::validate_arango_import_options(target);
                    } catch (const Error &error) {
                        fail("execution", error.what());
                    }
                }
                if (target.contains("password_env")) {
                    const auto variable = text(target, "password_env", "execution");
                    if (std::getenv(variable.c_str()) == nullptr)
                        fail("execution", "required environment variable is unset: " + variable);
                }
                if (backend != Backend::postgresql) {
                    const auto endpoint = text(target, "endpoint", "execution");
                    if ((!endpoint.starts_with("http://") && !endpoint.starts_with("https://")) ||
                        endpoint.find('@') != std::string::npos)
                        fail("execution", name + " endpoint must be HTTP(S) without embedded credentials");
                } else if (target.contains("port")) {
                    const auto port = number(target, "port", 5432, "execution");
                    if (port == 0 || port > 65535)
                        fail("execution", "PostgreSQL port must be in [1,65535]");
                }
                if (backend == Backend::neo4j) {
                    if (!target.contains("offline") || !target["offline"].is_boolean() ||
                        !target["offline"].get<bool>())
                        fail("execution", "Neo4J requires offline=true");
                    auto checked = target;
                    if (!checked.contains("runner"))
                        checked["runner"] = {"neo4j-admin"};
                    command(checked, "runner", true);
                    command(checked, "preflight_command", true);
                    command(checked, "start_command");
                }
            }
        }
    } // namespace

    GraphManifest load_graph_manifest(const fs::path &path, const InputAdapterRegistry *registry) {
        GraphManifest graph;
        graph.path = fs::weakly_canonical(fs::absolute(path));
        const auto root = json_file(graph.path, "manifest");
        if (!root.is_object() || root.value("format", Json()) != "polygraph.domain")
            fail("format", "expected polygraph.domain");
        if (!root.contains("version") || !root["version"].is_number_integer() || root["version"] != 1)
            fail("version", "only polygraph.domain version 1 is supported");
        graph.id = text(root, "id", "id");
        if (!root.contains("paths") || !root["paths"].is_object())
            fail("paths", "must be an object");
        const auto &paths = root["paths"];
        const auto resolve = [&](const std::string &key, bool required = true) -> fs::path {
            if (!required && !paths.contains(key))
                return {};
            const fs::path value = text(paths, key, "paths");
            return fs::weakly_canonical(value.is_absolute() ? value : graph.path.parent_path() / value);
        };
        graph.schema = resolve("schema");
        graph.runtime_data = resolve("data");
        graph.backend_config = resolve("backendConfig");
        graph.target_config = resolve("targetConfig", false);
        if (!root.contains("ingestion"))
            fail("ingestion",
                 "legacy runtime manifest has no native configuration; add an ingestion section and native paths");
        const auto &ingestion = root["ingestion"];
        keys(ingestion, {"adapter", "identity", "options"}, "ingestion");
        auto &o = graph.options;
        o.input_adapters = registry;
        o.adapter = text(ingestion, "adapter", "adapter");
        const InputAdapterDefinition *adapter = nullptr;
        try {
            adapter = &detail::input_adapters(o).get(o.adapter);
        } catch (const Error &error) {
            fail("adapter", error.what());
        }
        o.input = paths.contains("sourceData") ? resolve("sourceData") : graph.runtime_data;
        o.placement_config = resolve("placementConfig");
        if (root.contains("workspace") || !paths.contains("ingestionArtifacts")) {
            try {
                const auto workspace_root = graph_workspace_root(root, graph.path);
                o.output_dir = workspace_root / "ingestion";
                o.workspace = ArtifactWorkspace{workspace_root, graph.path, graph.runtime_data};
            } catch (const std::exception &error) {
                fail("workspace", error.what());
            }
        }
        if (paths.contains("ingestionArtifacts")) {
            o.output_dir = resolve("ingestionArtifacts");
            // Legacy overrides outside the namespace remain unchanged. Overrides inside
            // it still participate in the same ownership checks as default preparation.
            try {
                const auto workspace_root = graph_workspace_root(root, graph.path);
                const auto relative = o.output_dir.lexically_relative(workspace_root);
                if (!relative.empty() && *relative.begin() != "..")
                    o.workspace = ArtifactWorkspace{workspace_root, graph.path, graph.runtime_data};
                else
                    o.workspace.reset();
            } catch (const std::exception &) {
                if (root.contains("workspace"))
                    throw;
            }
        }
        o.partition_file = resolve("partitionFile", false);
        o.partition_map = resolve("partitionMap", false);
        o.configuration_files = {graph.path, graph.schema, graph.backend_config};
        if (!graph.target_config.empty())
            o.configuration_files.push_back(graph.target_config);
        const auto identity = ingestion.value("identity", Json::object());
        keys(identity, {"strategy"}, "identity");
        graph.identity_strategy =
                identity.contains("strategy") ? text(identity, "strategy", "identity") : strategy(*adapter);
        if (graph.identity_strategy != strategy(*adapter))
            fail("identity", "strategy does not match the selected adapter: " + graph.identity_strategy);
        const auto options = ingestion.value("options", Json::object());
        keys(options,
             {"partitioner", "seed", "threads", "memory-limit-mb", "chunk-size-mb", "max-record-mb", "partitions",
              "partition-threads", "partition-imbalance", "partition-preset", "relational-mapping"},
             "options");
        if (options.contains("relational-mapping")) {
            o.relational_schema = graph.schema;
            o.relational_mapping =
                    fs::absolute(graph.path).parent_path() / text(options, "relational-mapping", "options");
            o.relational_mapping = o.relational_mapping.lexically_normal();
            o.configuration_files.push_back(o.relational_mapping);
        }
        o.seed = number(options, "seed", o.seed);
        o.threads = number(options, "threads", o.threads);
        o.memory_limit_mb = number(options, "memory-limit-mb", o.memory_limit_mb);
        o.chunk_size_mb = number(options, "chunk-size-mb", o.chunk_size_mb);
        o.max_record_mb = number(options, "max-record-mb", o.max_record_mb);
        o.partition_options.count = number(options, "partitions", o.partition_options.count);
        o.partition_options.threads = number(options, "partition-threads", o.partition_options.threads);
        if (options.contains("partitioner"))
            o.partitioner = text(options, "partitioner", "options");
        if (options.contains("partition-preset"))
            o.partition_options.preset = text(options, "partition-preset", "options");
        if (options.contains("partition-imbalance")) {
            if (!options["partition-imbalance"].is_number())
                fail("options", "partition-imbalance must be numeric");
            o.partition_options.imbalance = options["partition-imbalance"].get<double>();
        }
        return graph;
    }

    Json validate_graph_manifest(const GraphManifest &graph, bool execution) {
        const auto &o = graph.options;
        try {
            detail::check_prepare_options(o);
        } catch (const Error &error) {
            fail("options", error.what());
        }
        if (o.workspace) {
            const auto &workspace = *o.workspace;
            const auto marker = workspace_reservation_path(workspace.root);
            if (fs::exists(marker) && read_json(marker) != workspace_owner(workspace.manifest, workspace.graph))
                fail("workspace", "artifact ownership collision: " + marker.string());
        }
        writable_output(o.output_dir);
        auto protected_paths = o.configuration_files;
        protected_paths.insert(protected_paths.end(), {o.input, o.placement_config, o.partition_file, o.partition_map});
        for (const auto &path: protected_paths) {
            if (path.empty())
                continue;
            const auto relative = path.lexically_relative(o.output_dir);
            if (!relative.empty() && *relative.begin() != "..")
                fail("output", "artifact directory contains source/configuration input: " + path.string());
        }
        if (!fs::exists(o.input))
            fail("input", "source is unavailable: " + o.input.string());
        std::vector<fs::path> inventory;
        Json metadata;
        const auto &adapter = detail::input_adapters(o).get(o.adapter);
        if (graph.identity_strategy != strategy(adapter))
            fail("identity", "strategy does not match the selected adapter");
        try {
            inventory = detail::input_paths(o);
            for (const auto &path: inventory)
                file(path, "input");
            metadata = adapter.inspect ? adapter.inspect(o) : Json::object();
        } catch (const std::exception &error) {
            fail("input", error.what());
        }
        const auto schema = check_schema(graph.schema);
        Placement placement;
        try {
            placement = load_placement(o.placement_config);
            (void) detail::load_partition_mapping(o, placement);
            if (!o.partition_file.empty())
                file(o.partition_file, "placement");
        } catch (const std::exception &error) {
            fail("placement", error.what());
        }
        if (std::ranges::find(placement.backends, Backend::arango) != placement.backends.end())
            for (const auto &type: schema.nodes)
                if (schema.edges.contains(type))
                    fail("schema", "Arango vertex/relationship collection name collision: " + type);
        for (const auto &[field, names]: std::array<std::pair<std::string, const std::set<std::string> *>, 2>{
                     {{"nodeSources", &schema.nodes}, {"relationshipSources", &schema.edges}}}) {
            if (placement.configuration.contains(field))
                for (const auto &[type, source]: placement.configuration[field].items()) {
                    (void) source;
                    if (!names->contains(type))
                        fail("placement", field + " references undeclared type: " + type);
                }
        }
        const auto backends = json_file(graph.backend_config, "backends");
        if (!backends.is_object() || !backends.contains("backends") || !backends["backends"].is_array())
            fail("backends", "runtime backendConfig requires a backends array");
        std::set<std::string> enabled;
        for (const auto &backend: backends["backends"]) {
            if (!backend.is_object() || (backend.contains("enabled") && !backend["enabled"].is_boolean()))
                fail("backends", "backend entries must be objects with boolean enabled flags");
            if (backend.value("enabled", true)) {
                const auto kind = text(backend, "kind", "backends");
                if (kind == "Neo4j" || kind == "Neo4J" || kind == "ArangoDB" || kind == "PostgreSQL")
                    enabled.insert(backend_name(parse_backend(kind)));
            }
        }
        for (const auto backend: placement.backends)
            if (!enabled.contains(backend_name(backend)))
                fail("backends", "runtime backendConfig has no enabled " + backend_name(backend));
        if (!graph.target_config.empty()) {
            try {
                (void) detail::checked_targets(json_file(graph.target_config, "execution"), placement.backends);
            } catch (const std::exception &error) {
                fail("execution", error.what());
            }
        }
        if (adapter.id_policy == InputIdPolicy::canonical && adapter.inspect) {
            if (!metadata.contains("idRouting") || !metadata["idRouting"].is_array())
                fail("identity", "canonical metadata requires idRouting");
            std::set<Backend> routed;
            for (const auto &name: metadata["idRouting"]) {
                try {
                    if (!name.is_string() || !routed.insert(parse_backend(name.get<std::string>())).second)
                        fail("identity", "duplicate or invalid idRouting entry");
                } catch (const std::exception &) {
                    fail("identity", "unsupported or duplicate idRouting entry");
                }
            }
            if (routed != std::set<Backend>(placement.backends.begin(), placement.backends.end()) ||
                metadata.value("idRoutingBits", Json(16)) != 16 || metadata.value("idPayloadBits", Json(48)) != 48)
                fail("identity", "canonical idRouting/layout differs from placement or the required 16/48 bits");
        }
        for (const auto &[field, names]: std::array<std::pair<std::string, const std::set<std::string> *>, 2>{
                     {{"nodeTypes", &schema.nodes}, {"relationshipTypes", &schema.edges}}}) {
            if (metadata.contains(field))
                for (const auto &type: metadata[field])
                    if (!type.is_string() || !names->contains(type.get<std::string>()))
                        fail("schema", "source references undeclared type in " + field);
        }
        if (adapter.id_policy == InputIdPolicy::ldbc_block31) {
            for (const auto &[field, fallback, types]:
                 std::array<std::tuple<std::string, std::string, const std::set<std::string> *>, 2>{
                         {{"nodeSources", "defaultNodeSource", &schema.nodes},
                          {"relationshipSources", "defaultRelationshipSource", &schema.edges}}})
                for (const auto &type: *types)
                    if (!placement.configuration.contains(fallback) &&
                        (!placement.configuration.contains(field) || !placement.configuration[field].contains(type)))
                        fail("placement", "no raw-source placement for type: " + type);
        }
        if (execution)
            execution_prerequisites(graph, placement);
        return {{"state", "valid"},
                {"graph", graph.id},
                {"adapter", o.adapter},
                {"identity_strategy", graph.identity_strategy},
                {"source_files", inventory.size()},
                {"output_dir", o.output_dir.string()},
                {"execution_prerequisites", execution},
                {"source_inspection", adapter.inspect ? "metadata" : "inventory-only"},
                {"not_checked",
                 {"record uniqueness/properties/endpoint closure", "target freshness/connectivity",
                  "operator command results", "future filesystem availability"}}};
    }

    Json native_executor_capabilities() {
        return {{"Neo4J",
#if POLYGRAPH_INGESTION_HAS_CURL
                 true
#else
                 false
#endif
                },
                {"ArangoDB",
#if POLYGRAPH_INGESTION_HAS_CURL
                 true
#else
                 false
#endif
                },
                {"PostgreSQL",
#if POLYGRAPH_INGESTION_HAS_LIBPQ
                 true
#else
                 false
#endif
                }};
    }
} // namespace polygraph::ingestion
