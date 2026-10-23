/**
 * @file include/cli/DomainManifest.hpp
 * @brief Loading of the domain.json runtime manifest.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace polygraph::cli {

struct DomainBfsDefaults {
    std::optional<std::string> start_uuid;
    std::optional<int> max_depth;
    std::optional<std::string> direction;
};

struct DomainRuntimeDefaults {
    std::uint64_t graph_version = 1;
    bool verbose = false;
};

struct DomainManifest {
    std::string id;
    std::string name;
    std::filesystem::path manifest_path;
    std::filesystem::path logical_graph_schema_path;
    std::filesystem::path data_path;
    std::filesystem::path backend_config_path;
    std::filesystem::path observability_config_path;
    std::optional<std::filesystem::path> native_id_map_path;
    std::optional<std::filesystem::path> cache_plan_path;
    DomainRuntimeDefaults runtime;
    DomainBfsDefaults bfs;
};

/**
 * Load a polygraph.domain version 1 manifest. Relative paths inside the manifest are
 * resolved relative to the directory containing domain.json.
 */
[[nodiscard]] DomainManifest load_domain_manifest(const std::filesystem::path& path);

} // namespace polygraph::cli
