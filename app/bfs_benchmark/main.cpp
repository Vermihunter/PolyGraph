/**
 * @file app/bfs_benchmark/main.cpp
 * @brief Command-line entry point for the in-process BFS benchmark.
 */

#include "BfsBenchmark.hpp"

#include "cli/BfsCli.hpp"
#include "cli/Cli.hpp"
#include "cli/CommandLine.hpp"

#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    polygraph::cli::CommandLine command_line("polygraph-bfs-benchmark");
    polygraph::cli::add_runtime_arguments(command_line);
    polygraph::cli::add_bfs_arguments(command_line);
    polygraph::cli::add_log_arguments(command_line);
    polygraph::benchmark::add_bfs_benchmark_arguments(command_line);

    try {
        command_line.parse(argc, argv);
        if (command_line.help_requested()) {
            std::cout << command_line;
            return 0;
        }
        return polygraph::benchmark::run_bfs_benchmark(command_line);
    } catch (const std::exception& error) {
        std::cerr << "polygraph-bfs-benchmark: " << error.what() << "\n\n" << command_line;
        return 1;
    } catch (...) {
        std::cerr << "polygraph-bfs-benchmark: unknown exception\n\n" << command_line;
        return 1;
    }
}
