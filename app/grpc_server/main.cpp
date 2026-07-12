/**
 * @file src/grpc_server/main.cpp
 * @brief Implementation for main in the gRPC server module.
 */

#include "cli/CommandLine.hpp"
#include "cli/CurlGlobalGuard.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "server/grpc/GrpcTraversalService.hpp"
#include "server/services/TraversalService.hpp"

#include <exception>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <optional>
#include <string>

namespace {

cpgdb::cli::CommandLine make_command_line() {
    cpgdb::cli::CommandLine command_line("grpc_server");
    command_line.add_value_option(
        "address", {"--address"}, "Listen address", std::string{"0.0.0.0:50051"});
    command_line.add_value_option("logical-graph-schema",
                                  {"-lgs", "--logical-graph-schema"},
                                  "Path to the logical graph schema JSON file",
                                  std::nullopt,
                                  true);
    command_line.add_value_option(
        "data", {"--data"}, "Path to the graph data JSON file", std::nullopt, true);
    command_line.add_value_option("backend-config",
                                  {"--backend-config"},
                                  "Path to the backend configuration JSON file",
                                  std::string{});
    command_line.add_value_option(
        "graph-version", {"--graph-version"}, "Logical graph version", std::string{"1"});
    command_line.add_flag_option("verbose", {"-v", "--verbose"}, "Enable verbose startup output");
    return command_line;
}

} // namespace

int main(int argc, char** argv) {
    auto command_line = make_command_line();
    try {
        command_line.parse(argc, argv);
        if (command_line.help_requested()) {
            std::cout << command_line;
            return 0;
        }

        std::optional<std::string> backend_config;
        if (const auto& value = command_line.string_value("backend-config"); !value.empty()) {
            backend_config = value;
        }

        const int graph_version = command_line.int_value("graph-version");
        if (graph_version < 0) {
            throw std::invalid_argument("--graph-version must be non-negative");
        }

        cpgdb::runtime::CurlGlobalGuard curl;
        cpgdb::cli::GraphRuntimeBuilder builder;
        auto context = builder.build(
            {.logical_graph_schema_path = command_line.string_value("logical-graph-schema"),
             .data_path = command_line.string_value("data"),
             .backend_config_path = std::move(backend_config),
             .graph_version = static_cast<std::uint64_t>(graph_version),
             .verbose = command_line.bool_value("verbose")});

        cpgdb::services::TraversalService traversal_service(*context);
        cpgdb::server::grpc_api::GrpcTraversalService grpc_service(traversal_service);

        const auto& address = command_line.string_value("address");
        ::grpc::ServerBuilder server_builder;
        server_builder.AddListeningPort(address, ::grpc::InsecureServerCredentials());
        server_builder.RegisterService(&grpc_service);

        auto server = server_builder.BuildAndStart();
        if (!server) {
            throw std::runtime_error("Failed to start gRPC server on " + address);
        }

        std::cout << "CPGDB gRPC server listening on " << address << '\n';
        server->Wait();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fatal gRPC server error: " << error.what() << "\n\n" << command_line;
        return 1;
    }
}
