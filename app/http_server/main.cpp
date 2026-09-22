/**
 * @file app/http_server/main.cpp
 * @brief Drogon HTTP server application entry point.
 */

#include "cli/CommandLine.hpp"
#include "cli/CurlGlobalGuard.hpp"

#include <drogon/drogon.h>

#include <exception>
#include <functional>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] polygraph::cli::CommandLine make_command_line() {
    polygraph::cli::CommandLine command_line("polygraph-http-server");
    command_line.add_value_option(
        "server-config",
        {"-c", "--server-config"},
        "Path to the Drogon server configuration JSON file",
        std::string{POLYGRAPH_PROJECT_ROOT} + "/config/drogon/http_server.full.json");
    return command_line;
}

void register_health_endpoint() {
    drogon::app().registerHandler(
        "/healthz",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            Json::Value body;
            body["status"] = "ok";
            callback(drogon::HttpResponse::newHttpJsonResponse(body));
        },
        {drogon::Get});
}

} // namespace

int main(int argc, char* argv[]) {
    auto command_line = make_command_line();

    try {
        command_line.parse(argc, argv);
        if (command_line.help_requested()) {
            std::cout << command_line;
            return 0;
        }

        polygraph::runtime::CurlGlobalGuard curl;
        register_health_endpoint();

        drogon::app()
            .loadConfigFile(command_line.string_value("server-config"))
            .run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "polygraph-http-server: " << error.what() << "\n\n"
                  << command_line;
        return 1;
    }
}
