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

#include "logging/sinks/JsonEventSerializer.hpp"
#include "logging/sinks/SpdlogEventSink.hpp"
#include "logging/sinks/TextEventSerializer.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

void add_jsonl_sink(
    cpgdb::EventLogger& event_logger,
    const std::string& path) {

    auto file_logger =
        spdlog::basic_logger_mt(
            "cpgdb-events-jsonl",
            path);

    file_logger->set_pattern("%v");
    file_logger->set_level(spdlog::level::trace);

    auto event_serializer = std::make_unique<cpgdb::JsonEventSerializer>();

    event_logger.add_sink(
        std::make_unique<cpgdb::SpdlogEventSink>(
            std::move(file_logger), std::move(event_serializer)));
}

void add_stdout_sink(cpgdb::EventLogger& event_logger) {
    auto stdout_logger =
        spdlog::stdout_color_mt("cpgdb");

    stdout_logger->set_level(spdlog::level::info);

    auto event_serializer = std::make_unique<cpgdb::TextEventSerializer>();

    event_logger.add_sink(
        std::make_unique<cpgdb::SpdlogEventSink>(
            std::move(stdout_logger), std::move(event_serializer)));
}

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

    auto log_options = cpgdb::cli::parse_log_options(program);
    cpgdb::logger = std::make_unique<cpgdb::EventLogger>(log_options.log_file);
    add_jsonl_sink(*cpgdb::logger, log_options.log_file);
 //   add_stdout_sink(*cpgdb::logger);

    try {

        auto ctx = cpgdb::cli::GraphRuntimeBuilder::build(cpgdb::cli::parse_runtime_config(program));

        cpgdb::BfsExecutor bfs(*ctx->placement_catalog);

        const cpgdb::NodeRef source = cpgdb::cli::parse_source(program, *ctx);
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
