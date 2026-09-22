/**
 * @file include/cli/BfsCli.hpp
 * @brief Declarations for bfs cli in the command-line and runtime configuration module.
 */

#pragma once

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "cli/CommandLine.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "data_model/NodeRef.hpp"
#include "domain/graph/LogicalGraphSchema.hpp"
#include "infrastructure/DatabaseContext.hpp"

#include <iosfwd>

namespace polygraph::cli {

void add_runtime_arguments(CommandLine& program);
void add_bfs_arguments(CommandLine& program);

[[nodiscard]] GraphRuntimeConfig parse_runtime_config(const CommandLine& args);
[[nodiscard]] NodeRef parse_source(const CommandLine& args, const DatabaseContext& context);
[[nodiscard]] ExpandSpec parse_expand_spec(const CommandLine& args,
                                           const domain::LogicalGraphSchema& schema);
[[nodiscard]] BfsOptions parse_bfs_options(const CommandLine& args);
void write_bfs_distances_csv(std::ostream& output, const BfsResult& result);

} // namespace polygraph::cli
