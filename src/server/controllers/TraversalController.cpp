/**
 * @file src/server/controllers/TraversalController.cpp
 * @brief Implementation for traversal controller in the server frontend module.
 */

#include "server/controllers/TraversalController.hpp"

#include "events/Events.hpp"
#include "serialization/BfsResultJsonSerializer.hpp"
#include "serialization/MinimalAdjacencyRowJsonSerializer.hpp"
#include "server/dto/BfsRequest.hpp"
#include "server/http/ErrorMapper.hpp"
#include "server/http/HttpRequestExecutor.hpp"
#include "server/http/JsonResponse.hpp"
#include "server/http/RequestParsing.hpp"
#include "server/services/ServicesPlugin.hpp"
#include "utilities/json.hpp"
#include "utilities/uuid.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <utility>

namespace polygraph::http::api::v1 {
    namespace {

        struct TimedBfsResult {
            BfsResult result;
            std::uint64_t duration_ns{};
        };

        TimedBfsResult run_bfs(const std::string &source_vertex_uuid, const dto::BfsRequest &bfs_request) {
            EVENT_SCOPE_INFO(bfs_layer, "BFS execution call", {"source-uuid", source_vertex_uuid});
            auto *plugin = drogon::app().getPlugin<polygraph::services::ServicesPlugin>();
            const auto started = std::chrono::steady_clock::now();
            auto result = plugin->traversalService().runBfs(source_vertex_uuid, bfs_request);
            const auto duration_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                            .count());
            EVENT_SCOPE_INFO_FIELD(bfs_layer, "vertices", result.distance.size());
            return TimedBfsResult{.result = std::move(result), .duration_ns = duration_ns};
        }

        std::vector<MinimalAdjacencyRow> run_get_neighbors(const std::string &source_vertex_uuid) {
            EVENT_SCOPE_INFO(neighbor_layer, "getNeighbors execution call", {"source-uuid", source_vertex_uuid});
            auto *plugin = drogon::app().getPlugin<polygraph::services::ServicesPlugin>();
            auto source_adjacency_row = plugin->traversalService().getNeighbors(source_vertex_uuid);

            EVENT_SCOPE_INFO_FIELD(neighbor_layer, "neighbors", source_adjacency_row[0].neighbors.size());
            return source_adjacency_row;
        }

    } // namespace

    void TraversalController::runBfs(const drogon::HttpRequestPtr &request,
                                     std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &source_vertex_uuid) {
        HttpRequestExecutor::executeRequest(std::move(callback), "/api/v1/graphs/{id}/bfs", "POST", [&] {
            const auto &body = parse::jsonBody(request);
            const auto bfs_request = dto::BfsRequest::fromJson(body);
            auto timed = run_bfs(source_vertex_uuid, bfs_request);
            auto *plugin = drogon::app().getPlugin<polygraph::services::ServicesPlugin>();
            const auto requested_source = global_id_from_string(source_vertex_uuid);
            const auto resolved_source = plugin->dbContext().resolve_input_id(requested_source);

            nlohmann::json response{{"sourceVertexId", global_id_to_string(resolved_source)},
                                    {"requestedSourceVertexId", source_vertex_uuid},
                                    {"sourceVertexIdWasRemapped", resolved_source != requested_source},
                                    {"maxDepth", bfs_request.maxDepth},
                                    {"vertexCount", timed.result.distance.size()},
                                    {"predecessorCount", timed.result.predecessor.size()},
                                    {"bfsDurationNs", timed.duration_ns}};
            if (bfs_request.responseMode == "full") {
                response["result"] = std::move(timed.result);
            }
            return jsonOk(response);
        });
    }

    void TraversalController::getNeighbors(const drogon::HttpRequestPtr &request,
                                           std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                           const std::string &source_vertex_uuid) {
        HttpRequestExecutor::executeRequest(std::move(callback), "/api/v1/graphs/{id}/neighbors", "POST", [&] {
            (void) request;
            auto source_adjacency_row = run_get_neighbors(source_vertex_uuid);
            return jsonOk(source_adjacency_row[0]);
        });
    }

} // namespace polygraph::http::api::v1
