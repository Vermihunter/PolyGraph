//
// Created by akos on 7. 5. 2026.
//

#include <exception>
#include <iostream>

#include <argparse/argparse.hpp>

#include "algorithms/bfs/BfsExecutor.hpp"
#include "cli/BfsCli.hpp"
#include "cli/CurlGlobalGuard.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "logging/LoggingSetup.hpp"
#include "utilities/Profiling.hpp"
#include "utilities/logging.hpp"

int main(int argc, char** argv) {
    cpgdb::runtime::CurlGlobalGuard curl;

    argparse::ArgumentParser program("graph_db");
    cpgdb::cli::add_runtime_arguments(program);
    cpgdb::cli::add_bfs_arguments(program);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << '\n';
        std::cerr << program;
        return 1;
    }

    cpgdb::cli::parse_log_options(program);

    try {
        cpgdb::cli::GraphRuntimeBuilder builder;
        auto ctx = builder.build(cpgdb::cli::parse_runtime_config(program));

        BfsExecutor bfs(*ctx->placement_catalog);

        const NodeRef source = cpgdb::cli::parse_source(program, *ctx->schema);
        const ExpandSpec expand_spec = cpgdb::cli::parse_expand_spec(program, *ctx->schema);
        const BfsOptions options = cpgdb::cli::parse_bfs_options(program);

        const auto result = bfs.run(source, expand_spec, options);
        cpgdb::cli::write_bfs_distances_csv(std::cout, result);

        //cpgdb::prof::Profiler::dump(std::cerr);
        return 0;
    } catch (...) {
        cpgdb::prof::Profiler::dump(std::cerr);
        throw;
    }
}
