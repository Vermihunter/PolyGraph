/**
 * @file src/bfs/main.cpp
 * @brief Implementation for main in the BFS executable module.
 */

//
// Created by akos on 7. 5. 2026.
//

#include "algorithms/bfs/BfsExecutor.hpp"
#include "cli/BfsCli.hpp"
#include "cli/Cli.hpp"
#include "cli/CurlGlobalGuard.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "logging/LoggingSetup.hpp"
#include "utilities/Profiling.hpp"
#include "utilities/logging.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    cpgdb::runtime::CurlGlobalGuard curl;

    cpgdb::cli::CommandLine program("bfs");
    cpgdb::cli::add_runtime_arguments(program);
    cpgdb::cli::add_bfs_arguments(program);
    cpgdb::cli::add_log_arguments(program);

    try {
        program.parse(argc, argv);
        if (program.help_requested()) {
            std::cout << program;
            return 0;
        }
    } catch (const std::exception& err) {
        std::cerr << err.what() << '\n';
        std::cerr << program;
        return 1;
    }

    cpgdb::cli::parse_log_options(program);

    try {

        auto ctx = cpgdb::cli::GraphRuntimeBuilder::build(cpgdb::cli::parse_runtime_config(program));

        cpgdb::BfsExecutor bfs(*ctx->placement_catalog);

        const cpgdb::NodeRef source = cpgdb::cli::parse_source(program, *ctx->schema);
        const cpgdb::ExpandSpec expand_spec = cpgdb::cli::parse_expand_spec(program, *ctx->schema);
        const cpgdb::BfsOptions options = cpgdb::cli::parse_bfs_options(program);

        const auto result = bfs.run(source, expand_spec, options);
        cpgdb::cli::write_bfs_distances_csv(std::cout, result);

        // cpgdb::prof::Profiler::dump(std::cerr);
        return 0;
    } catch (...) {
        cpgdb::prof::Profiler::dump(std::cerr);
        throw;
    }
}
