/**
 * @file src/server/controllers/TraversalController.cpp
 * @brief Implementation for traversal controller in the server frontend module.
 */

#include "server/controllers/TraversalController.hpp"

#include "logging/level/Info.hpp"
#include "serialization/BfsResultJsonSerializer.hpp"
#include "serialization/MinimalAdjacencyRowJsonSerializer.hpp"
#include "server/dto/BfsRequest.hpp"
#include "server/http/ErrorMapper.hpp"
#include "server/http/HttpRequestExecutor.hpp"
#include "server/http/JsonResponse.hpp"
#include "server/http/RequestParsing.hpp"
#include "server/services/ServicesPlugin.hpp"
#include "utilities/json.hpp"
#include "utilities/logging.hpp"

#include <exception>
#include <utility>

namespace cpgdb::http::api::v1 {
    namespace {
        BfsResult _runBFS(const std::string &source_vertex_uuid, const dto::BfsRequest &bfs_request) {
            EVENT_SCOPE_INFO(bfs_layer, "BFS execution call", {"source-uuid", source_vertex_uuid});
            auto *plugin = drogon::app().getPlugin<cpgdb::services::ServicesPlugin>();
            auto result = plugin->traversalService().runBfs(source_vertex_uuid, bfs_request);
            EVENT_SCOPE_INFO_FIELD(bfs_layer, "vertices", result.distance.size());

            return result;
        }

        std::vector<MinimalAdjacencyRow> _runGetNeighbors(const std::string &source_vertex_uuid,
                                                          const std::string &source_vertex_name) {
            EVENT_SCOPE_INFO(neighbor_layer, "getNeighbors execution call", {"source-uuid", source_vertex_uuid});
            auto *plugin = drogon::app().getPlugin<cpgdb::services::ServicesPlugin>();
            const auto &db_context = plugin->dbContext();

            TypeId source_vertex_type_id = db_context.schema->node_type_id(source_vertex_name);
            auto source_adjacency_row =
                    plugin->traversalService().getNeighbors(source_vertex_uuid, source_vertex_type_id);

            EVENT_SCOPE_INFO_FIELD(neighbor_layer, "neighbors", source_adjacency_row[0].neighbors.size());
            return source_adjacency_row;
        }
    } // namespace

    void TraversalController::runBfs(const drogon::HttpRequestPtr &request,
                                     std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &source_vertex_uuid) {

        HttpRequestExecutor::executeRequest(std::move(callback), [&] {
            const auto &body = parse::jsonBody(request);
            const auto bfs_request = dto::BfsRequest::fromJson(body);
            const BfsResult result = _runBFS(source_vertex_uuid, bfs_request);

            return jsonOk(nlohmann::json{
                    {"sourceVertexId", source_vertex_uuid}, {"maxDepth", bfs_request.maxDepth}, {"result", result}});
        });
    }

    void TraversalController::getNeighbors(const drogon::HttpRequestPtr &request,
                                           std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                           const std::string &source_vertex_uuid) {

        HttpRequestExecutor::executeRequest(std::move(callback), [&] {
            const auto json = request->getJsonObject();
            const std::string name = (*json)["source_vertex_type"].asString();
            auto source_adjacency_row = _runGetNeighbors(source_vertex_uuid, name);

            return jsonOk(source_adjacency_row[0]);
        });
    }

} // namespace cpgdb::http::api::v1
