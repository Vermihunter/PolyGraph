/**
 * @file include/cli/GraphRuntimeBuilder.hpp
 * @brief Declarations for graph runtime builder in the command-line and runtime configuration
 * module.
 */

#pragma once

#include "infrastructure/DatabaseContext.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace polygraph::cli {

struct GraphRuntimeConfig {
    std::string logical_graph_schema_path;
    std::string data_path;
    std::optional<std::string> backend_config_path;
    std::optional<std::string> native_id_map_path;
    std::optional<std::string> cache_plan_path;
    std::uint64_t graph_version = 1;
    bool verbose = false;
};

class GraphRuntimeBuilder {
  public:
    [[nodiscard]] static std::shared_ptr<DatabaseContext> build(const GraphRuntimeConfig& config);
};

} // namespace polygraph::cli
