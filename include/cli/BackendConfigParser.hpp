/**
 * @file include/cli/BackendConfigParser.hpp
 * @brief Declarations for backend config parser in the command-line and runtime configuration
 * module.
 */

#ifndef CPGDB_CLI_BACKEND_CONFIG_PARSER_HPP
#define CPGDB_CLI_BACKEND_CONFIG_PARSER_HPP

#include "domain/graph/LogicalGraphSchema.hpp"
#include "infrastructure/BackendCapabilities.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "utilities/json.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cpgdb::config {

[[nodiscard]] std::string backend_id_to_string(BackendId id);
[[nodiscard]] BackendId backend_id_from_string(const std::string& value);
[[nodiscard]] BackendKind backend_kind_from_string(const std::string& value);
[[nodiscard]] std::string backend_kind_to_string(BackendKind kind);
[[nodiscard]] BackendConnectionConfig
parse_connection_config(const json& backend_json, BackendId id, BackendKind kind);
[[nodiscard]] BackendDescriptor
parse_descriptor(const json& backend_json,
                 BackendId id,
                 BackendKind kind,
                 const domain::LogicalGraphSchema& schema,
                 const std::unordered_map<BackendId, std::unordered_set<TypeId>>& nodes_by_backend,
                 const std::unordered_map<BackendId, std::unordered_set<TypeId>>& edges_by_backend,
                 std::uint64_t graph_version);

[[nodiscard]] bool backend_json_enabled(const json& backend_json);
[[nodiscard]] json load_backend_config_file_or_default(const std::optional<std::string>& path);
[[nodiscard]] const json& backend_array_or_throw(const json& root);
[[nodiscard]] std::vector<BackendId> uuid_routing_backends(const json& root);
[[nodiscard]] bool backend_enabled(const DatabaseContext& context, BackendId id);
void print_backend_summary(const DatabaseContext& context, std::ostream& output);

} // namespace cpgdb::config

#endif // CPGDB_CLI_BACKEND_CONFIG_PARSER_HPP
