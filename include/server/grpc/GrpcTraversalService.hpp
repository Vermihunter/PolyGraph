//
// Created by akos on 8. 7. 2026.
//

#ifndef CPGDB_GRPCTRAVERSALSERVICE_HPP
#define CPGDB_GRPCTRAVERSALSERVICE_HPP

#include <grpcpp/grpcpp.h>

#include "bfs_stream.grpc.pb.h"
#include "server/services/TraversalService.hpp"

namespace cpgdb::server_rpc {
    namespace pb = ::cpgdb::grpc::v1;

    class GrpcTraversalService final
        : public pb::TraversalService::Service {
    public:
        explicit GrpcTraversalService(
            ::services::TraversalService& traversalService
        );

        ::grpc::Status StreamBfs(
            ::grpc::ServerContext* context,
            const pb::BfsStreamRequest* request,
            ::grpc::ServerWriter<pb::BfsLayer>* writer
        ) override;

    private:
        ::services::TraversalService& traversalService_;
    };
}

#endif //CPGDB_GRPCTRAVERSALSERVICE_HPP
