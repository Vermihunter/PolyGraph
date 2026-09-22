/**
 * @file src/server/grpc/GrpcTraversalService.cpp
 * @brief Implementation for grpc traversal service in the server frontend module.
 */

#include "server/grpc/GrpcTraversalService.hpp"
#include "utilities/uuid.hpp"

#include <exception>
#include <stdexcept>
#include <string>

namespace polygraph::server::grpc_api {
    namespace {

        std::string uuid_to_string(const GlobalId &id) { return global_id_to_string(id); }

        void fill_proto_layer(pb::BfsLayer &out, const std::string &graph_id, const BfsLayerEvent &layer) {
            out.set_graph_id(graph_id);
            out.set_depth(layer.depth);
            out.set_done(false);

            for (const auto &vertex: layer.vertices) {
                auto *proto_vertex = out.add_vertices();
                proto_vertex->set_id(uuid_to_string(vertex.global_id));
                proto_vertex->set_type(std::to_string(vertex.type));
                proto_vertex->set_distance(layer.depth);
            }

            for (const auto &edge: layer.tree_edges) {
                auto *proto_edge = out.add_tree_edges();
                proto_edge->set_source_id(uuid_to_string(edge.source.global_id));
                proto_edge->set_target_id(uuid_to_string(edge.target.global_id));
            }
        }

    } // namespace

    GrpcTraversalService::GrpcTraversalService(services::TraversalService &traversal_service) noexcept :
        traversal_service_(traversal_service) {}

    ::grpc::Status GrpcTraversalService::StreamBfs(::grpc::ServerContext *context, const pb::BfsStreamRequest *request,
                                                   ::grpc::ServerWriter<pb::BfsLayer> *writer) {
        try {
            if (request->graph_id().empty()) {
                return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "graph_id is required");
            }

            if (request->source_vertex_id().empty()) {
                return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "source_vertex_id is required");
            }

            (void) traversal_service_.streamBfs(request->graph_id(), request->source_vertex_id(), request->max_depth(),
                                                request->return_predecessors(), request->allow_native_pushdown(),
                                                request->allow_partial_acceleration(), [&](const BfsLayerEvent &layer) {
                                                    if (context->IsCancelled()) {
                                                        throw std::runtime_error("client cancelled StreamBfs");
                                                    }

                                                    pb::BfsLayer proto_layer;
                                                    fill_proto_layer(proto_layer, request->graph_id(), layer);

                                                    if (!writer->Write(proto_layer)) {
                                                        throw std::runtime_error("client stopped reading StreamBfs");
                                                    }
                                                });

            pb::BfsLayer done;
            done.set_graph_id(request->graph_id());
            done.set_done(true);
            if (!writer->Write(done)) {
                return ::grpc::Status(::grpc::StatusCode::CANCELLED, "client stopped reading before completion marker");
            }

            return ::grpc::Status::OK;
        } catch (const std::exception &e) {
            if (context->IsCancelled()) {
                return ::grpc::Status(::grpc::StatusCode::CANCELLED, e.what());
            }

            return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
        } catch (...) {
            return ::grpc::Status(::grpc::StatusCode::UNKNOWN, "unknown StreamBfs error");
        }
    }

} // namespace polygraph::server::grpc_api
