/**
 * @file src/cli/DomainManifest.cpp
 * @brief Loading and validation of the domain.json runtime manifest.
 */

#include "cli/DomainManifest.hpp"

#include "utilities/json.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace polygraph::cli {
    namespace {

        [[nodiscard]] std::filesystem::path project_root() {
#ifdef POLYGRAPH_PROJECT_ROOT
            return std::filesystem::path{POLYGRAPH_PROJECT_ROOT};
#else
            return std::filesystem::current_path();
#endif
        }

        [[nodiscard]] std::filesystem::path resolve_manifest_path(std::filesystem::path path) {
            if (path.empty()) {
                throw std::runtime_error("Domain manifest path must not be empty");
            }
            if (path.is_relative()) {
                path = project_root() / path;
            }
            return std::filesystem::weakly_canonical(path);
        }

        [[nodiscard]] std::filesystem::path resolve_domain_path(const std::filesystem::path &base,
                                                                const std::string &raw) {
            std::filesystem::path path{raw};
            if (path.is_relative()) {
                path = base / path;
            }
            return std::filesystem::weakly_canonical(path);
        }

        [[nodiscard]] std::string required_string(const json &object, const std::string_view key,
                                                  const std::string_view context) {
            const auto iterator = object.find(std::string(key));
            if (iterator == object.end() || !iterator->is_string() || iterator->get<std::string>().empty()) {
                throw std::runtime_error(std::string(context) + "." + std::string(key) + " must be a non-empty string");
            }
            return iterator->get<std::string>();
        }

        [[nodiscard]] std::optional<std::string> optional_string(const json &object, const std::string_view key) {
            const auto iterator = object.find(std::string(key));
            if (iterator == object.end() || iterator->is_null()) {
                return std::nullopt;
            }
            if (!iterator->is_string()) {
                throw std::runtime_error(std::string(key) + " must be a string");
            }
            const auto value = iterator->get<std::string>();
            return value.empty() ? std::nullopt : std::optional<std::string>{value};
        }

        [[nodiscard]] json read_json(const std::filesystem::path &path) {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("Could not open domain manifest: " + path.string());
            }
            try {
                return json::parse(input);
            } catch (const json::parse_error &error) {
                throw std::runtime_error("Invalid domain manifest JSON in " + path.string() + ": " + error.what());
            }
        }

    } // namespace

    DomainManifest load_domain_manifest(const std::filesystem::path &path) {
        const auto manifest_path = resolve_manifest_path(path);
        const auto root = read_json(manifest_path);

        if (!root.is_object()) {
            throw std::runtime_error("Domain manifest root must be a JSON object: " + manifest_path.string());
        }
        if (root.value("format", std::string{}) != "polygraph.domain") {
            throw std::runtime_error("Unsupported domain manifest format in " + manifest_path.string());
        }
        if (root.value("version", 0) != 1) {
            throw std::runtime_error("Unsupported domain manifest version in " + manifest_path.string());
        }

        const auto paths_iterator = root.find("paths");
        if (paths_iterator == root.end() || !paths_iterator->is_object()) {
            throw std::runtime_error("domain.paths must be an object in " + manifest_path.string());
        }

        const auto &paths = *paths_iterator;
        const auto base = manifest_path.parent_path();
        DomainManifest result;
        result.id = root.value("id", manifest_path.parent_path().filename().string());
        result.name = root.value("name", result.id);
        result.manifest_path = manifest_path;
        result.logical_graph_schema_path = resolve_domain_path(base, required_string(paths, "schema", "domain.paths"));
        result.data_path = resolve_domain_path(base, required_string(paths, "data", "domain.paths"));
        result.backend_config_path = resolve_domain_path(base, required_string(paths, "backendConfig", "domain.paths"));

        if (const auto native_id_map = optional_string(paths, "nativeIdMap")) {
            result.native_id_map_path = resolve_domain_path(base, *native_id_map);
        }
        if (const auto cache_plan = optional_string(paths, "cachePlan")) {
            result.cache_plan_path = resolve_domain_path(base, *cache_plan);
        }

        const auto observability = optional_string(paths, "observabilityConfig");
        result.observability_config_path =
                observability ? resolve_domain_path(base, *observability)
                              : resolve_domain_path(project_root(), "config/observability/observability.json");

        const auto runtime = root.value("runtime", json::object());
        if (!runtime.is_object()) {
            throw std::runtime_error("domain.runtime must be an object");
        }
        result.runtime.graph_version = runtime.value("graphVersion", std::uint64_t{1});
        result.runtime.verbose = runtime.value("verbose", false);

        const auto defaults = root.value("defaults", json::object());
        if (!defaults.is_object()) {
            throw std::runtime_error("domain.defaults must be an object");
        }
        const auto bfs = defaults.value("bfs", json::object());
        if (!bfs.is_object()) {
            throw std::runtime_error("domain.defaults.bfs must be an object");
        }
        result.bfs.start_uuid = optional_string(bfs, "startUuid");
        if (bfs.contains("maxDepth")) {
            if (!bfs.at("maxDepth").is_number_integer()) {
                throw std::runtime_error("domain.defaults.bfs.maxDepth must be an integer");
            }
            result.bfs.max_depth = bfs.at("maxDepth").get<int>();
        }
        result.bfs.direction = optional_string(bfs, "direction");
        return result;
    }

} // namespace polygraph::cli
