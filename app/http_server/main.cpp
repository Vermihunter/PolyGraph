/**
 * @file src/http_server/main.cpp
 * @brief Implementation for main in the HTTP server module.
 */

#include "cli/BfsCli.hpp"
#include "cli/Cli.hpp"
#include "cli/CommandLine.hpp"
#include "cli/CurlGlobalGuard.hpp"
#include "logging/EventLogger.hpp"
#include "logging/sinks/JsonEventSerializer.hpp"
#include "logging/sinks/SpdlogEventSink.hpp"
#include "logging/sinks/TextEventSerializer.hpp"
#include "server/controllers/TraversalController.hpp"
#include "utilities/logging.hpp"

#include <drogon/drogon.h>
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

    cpgdb::cli::CommandLine program("http-server");
    cpgdb::cli::add_log_arguments(program);

    program.add_value_option("server-config",
                             {"-c", "--server-config"},
                             "Path to the server config JSON file",
                             std::string(CPGDB_PROJECT_ROOT) + "/config/http_server.full.json");

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
    add_stdout_sink(*cpgdb::logger);

    drogon::app().registerHandler(
        "/healthz",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            Json::Value body;
            body["status"] = "ok";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
            callback(resp);
        },
        {drogon::Get});

    //const std::string configPath = argc >= 2 ? argv[1] : "../../config/http_server.full.json";

    drogon::app().loadConfigFile(program.string_value("server-config")).run();
}