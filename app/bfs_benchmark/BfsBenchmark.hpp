/**
 * @file app/bfs_benchmark/BfsBenchmark.hpp
 * @brief In-process repeated BFS benchmark orchestration.
 */

#pragma once

#include "cli/CommandLine.hpp"

namespace polygraph::benchmark {

/** Register benchmark-only command-line arguments. */
void add_bfs_benchmark_arguments(cli::CommandLine& command_line);

/**
 * Build the graph runtime once, then run every requested BFS directly through
 * BfsExecutor while retaining the loaded GPU projection between executions.
 */
[[nodiscard]] int run_bfs_benchmark(const cli::CommandLine& command_line);

} // namespace polygraph::benchmark
