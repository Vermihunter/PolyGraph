/**
 * @file include/cli/Cli.hpp
 * @brief Shared observability command-line options.
 */
#pragma once

#include "cli/CommandLine.hpp"
#include "observability/ObservabilityConfig.hpp"

namespace polygraph::cli {

void add_log_arguments(CommandLine& program);

/** Load event/logging/metrics configuration and apply CLI overrides. */
[[nodiscard]] observability::ObservabilityConfig parse_observability_config(const CommandLine& args);

[[nodiscard]] observability::EventGatheringOptions parse_event_options(const CommandLine& args);
[[nodiscard]] observability::LoggingOptions parse_log_options(const CommandLine& args);

} // namespace polygraph::cli
