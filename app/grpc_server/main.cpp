/**
 * @file app/grpc_server/main.cpp
 * @brief gRPC traversal server application entry point.
 */

#include "cli/BfsCli.hpp"
#include "cli/Cli.hpp"
#include "cli/CurlGlobalGuard.hpp"
#include "events/EventSetup.hpp"
#include "events/Events.hpp"
#include "logging/Log.hpp"
#include "logging/LoggingSetup.hpp"
#include "server/grpc/GrpcTraversalService.hpp"
#include "server/services/TraversalService.hpp"

#include <grpcpp/grpcpp.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    polygraph::cli::CommandLine command_line("polygraph-grpc-server");
    polygraph::cli::add_runtime_arguments(command_line);
    polygraph::cli::add_log_arguments(command_line);
    command_line.add_value_option(
        "listen-address",
        {"--listen-address"},
        "gRPC listen address",
        std::string{"0.0.0.0:50051"});

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
        const auto context = polygraph::cli::GraphRuntimeBuilder::build(
            polygraph::cli::parse_runtime_config(command_line));

        polygraph::services::TraversalService traversal(*context);
        polygraph::server::grpc_api::GrpcTraversalService service(traversal);

        const auto& address = command_line.string_value("listen-address");
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);

        const auto server = builder.BuildAndStart();
        if (!server) {
            throw std::runtime_error("Could not start gRPC server on " + address);
        }

        LOG_INFO("gRPC server listening on {}", address);
        EVENT_INFO("grpc.server.started", {"listen_address", address});
        server->Wait();
        EVENT_INFO("grpc.server.stopped", {"listen_address", address});
        LOG_INFO("gRPC server stopped");

        polygraph::events::shutdown();
        polygraph::logging::shutdown();
        return 0;
    } catch (const std::exception& error) {
        EVENT_ERROR("grpc.server.failed", {"error", error.what()});
        LOG_ERROR("gRPC server failed: {}", error.what());
        polygraph::events::shutdown();
        polygraph::logging::shutdown();
        std::cerr << "polygraph-grpc-server: " << error.what() << "\n\n" << command_line;
        return 1;
    } catch (...) {
        EVENT_ERROR("grpc.server.failed", {"error", "unknown exception"});
        LOG_ERROR("gRPC server failed with an unknown exception");
        polygraph::events::shutdown();
        polygraph::logging::shutdown();
        std::cerr << "polygraph-grpc-server: unknown exception\n\n" << command_line;
        return 1;
    }
}
