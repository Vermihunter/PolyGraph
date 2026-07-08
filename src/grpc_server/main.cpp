#include <grpcpp/grpcpp.h>

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "cli/CurlGlobalGuard.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "server/grpc/GrpcTraversalService.hpp"
#include "server/services/TraversalService.hpp"

namespace {

struct Args {
    std::string address = "0.0.0.0:50051";
    std::string logical_graph_schema;
    std::string data;
    std::optional<std::string> backend_config;
    std::uint64_t graph_version = 1;
    bool verbose = false;
};

void print_usage(const char* program) {
    std::cerr
        << "Usage:\n  " << program << " \\\n"
        << "    --logical-graph-schema <schema.json> \\\n"
        << "    --data <data.json> \\\n"
        << "    [--backend-config <backends.json>] \\\n"
        << "    [--address 0.0.0.0:50051] \\\n"
        << "    [--graph-version 1] [--verbose]\n";
}

Args parse_args(int argc, char** argv) {
    Args args;

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];

        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + name);
            }
            return argv[++i];
        };

        if (key == "--address") {
            args.address = need_value("--address");
        } else if (key == "--logical-graph-schema" || key == "-lgs") {
            args.logical_graph_schema = need_value("--logical-graph-schema");
        } else if (key == "--data") {
            args.data = need_value("--data");
        } else if (key == "--backend-config") {
            args.backend_config = need_value("--backend-config");
        } else if (key == "--graph-version") {
            args.graph_version = std::stoull(need_value("--graph-version"));
        } else if (key == "--verbose" || key == "-v") {
            args.verbose = true;
        } else if (key == "--help" || key == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }

    if (args.logical_graph_schema.empty()) {
        throw std::runtime_error("--logical-graph-schema is required");
    }

    if (args.data.empty()) {
        throw std::runtime_error("--data is required");
    }

    return args;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Args args = parse_args(argc, argv);

        cpgdb::runtime::CurlGlobalGuard curl;

        cpgdb::cli::GraphRuntimeConfig runtime_config{
            .logical_graph_schema_path = args.logical_graph_schema,
            .data_path = args.data,
            .backend_config_path = args.backend_config,
            .graph_version = args.graph_version,
            .verbose = args.verbose
        };

        cpgdb::cli::GraphRuntimeBuilder builder;
        auto ctx = builder.build(runtime_config);
        services::TraversalService traversal_service(*ctx);
        cpgdb::server_rpc::GrpcTraversalService grpc_service(traversal_service);

        grpc::ServerBuilder server_builder;
        server_builder.AddListeningPort(args.address, grpc::InsecureServerCredentials());
        server_builder.RegisterService(&grpc_service);

        auto server = server_builder.BuildAndStart();
        if (!server) {
            throw std::runtime_error("Failed to start gRPC server on " + args.address);
        }

        std::cout << "CPGDB gRPC server listening on " << args.address << '\n';
        server->Wait();

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal gRPC server error: " << e.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
