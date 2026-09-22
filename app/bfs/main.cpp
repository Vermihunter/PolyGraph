/**
 * @file app/bfs/main.cpp
 * @brief Command-line BFS application entry point.
 */

#include "algorithms/bfs/BfsExecutor.hpp"
#include "cli/BfsCli.hpp"
#include "cli/Cli.hpp"
#include "cli/CurlGlobalGuard.hpp"
#include "events/EventSetup.hpp"
#include "events/Events.hpp"
#include "logging/Log.hpp"
#include "logging/LoggingSetup.hpp"
#include "utilities/Profiling.hpp"

#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char* argv[]) {
    polygraph::cli::CommandLine command_line("polygraph-bfs");
    polygraph::cli::add_runtime_arguments(command_line);
    polygraph::cli::add_bfs_arguments(command_line);
    polygraph::cli::add_log_arguments(command_line);

    try {
        command_line.parse(argc, argv);
        if (command_line.help_requested()) {
            std::cout << command_line;
            return 0;
        }

        const auto observability = polygraph::cli::parse_observability_config(command_line);
        polygraph::logging::install_logger(observability.logging);
        polygraph::events::install_collector(observability.events);

        polygraph::runtime::CurlGlobalGuard curl;
        LOG_INFO("Starting BFS CLI");
        EVENT_INFO("bfs.cli.starting");

        const auto context = polygraph::cli::GraphRuntimeBuilder::build(
            polygraph::cli::parse_runtime_config(command_line));

        polygraph::BfsExecutor executor(*context->placement_catalog);
        const auto result = executor.run(
            polygraph::cli::parse_source(command_line, *context),
            polygraph::cli::parse_expand_spec(command_line, *context->schema),
            polygraph::cli::parse_bfs_options(command_line));

        const auto& output_file = command_line.string_value("output-file");
        if (output_file.empty()) {
            polygraph::cli::write_bfs_distances_csv(std::cout, result);
        } else {
            std::ofstream output(output_file);
            if (!output) {
                throw std::runtime_error("Could not open BFS output file: " + output_file);
            }
            polygraph::cli::write_bfs_distances_csv(output, result);
            LOG_INFO("BFS result written to {}", output_file);
        }
        EVENT_INFO("bfs.cli.completed", {"vertices", result.distance.size()});
        LOG_INFO("BFS completed with {} discovered vertices", result.distance.size());

        polygraph::events::shutdown();
        polygraph::logging::shutdown();
        return 0;
    } catch (const std::exception& error) {
        EVENT_ERROR("bfs.cli.failed", {"error", error.what()});
        LOG_ERROR("BFS CLI failed: {}", error.what());
        polygraph::prof::Profiler::dump(std::cerr);
        polygraph::events::shutdown();
        polygraph::logging::shutdown();
        std::cerr << "polygraph-bfs: " << error.what() << '\n' << command_line;
        return 1;
    } catch (...) {
        EVENT_ERROR("bfs.cli.failed", {"error", "unknown exception"});
        LOG_ERROR("BFS CLI failed with an unknown exception");
        polygraph::prof::Profiler::dump(std::cerr);
        polygraph::events::shutdown();
        polygraph::logging::shutdown();
        std::cerr << "polygraph-bfs: unknown exception\n" << command_line;
        return 1;
    }
}
