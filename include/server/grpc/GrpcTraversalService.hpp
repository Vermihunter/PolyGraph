/**
 * @file include/server/grpc/GrpcTraversalService.hpp
 * @brief Declarations for grpc traversal service in the server frontend module.
 */

#pragma once

#include "bfs_stream.grpc.pb.h"
#include "server/services/TraversalService.hpp"

#include <grpcpp/grpcpp.h>

namespace polygraph::server::grpc_api {

namespace pb = ::polygraph::grpc::v1;

class GrpcTraversalService final : public pb::TraversalService::Service {
  public:
    explicit GrpcTraversalService(services::TraversalService& traversal_service) noexcept;

    ::grpc::Status StreamBfs(::grpc::ServerContext* context,
                             const pb::BfsStreamRequest* request,
                             ::grpc::ServerWriter<pb::BfsLayer>* writer) override;

  private:
    services::TraversalService& traversal_service_;
};

} // namespace polygraph::server::grpc_api
