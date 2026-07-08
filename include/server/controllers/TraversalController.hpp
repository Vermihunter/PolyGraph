//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_TRAVERSALCONTROLLER_HPP
#define CPGDB_TRAVERSALCONTROLLER_HPP

#include <drogon/drogon.h>

#include "server/dto/BfsRequest.hpp"
#include "server/http/RequestParsing.hpp"
#include "server/services/ServicesPlugin.hpp"
#include "utilities/json.hpp"
#include "serialization/BfsResultJsonSerializer.hpp"

namespace http::api::v1 {

    class TraversalController final
        : public drogon::HttpController<TraversalController> {
    public:
        METHOD_LIST_BEGIN

        ADD_METHOD_TO(
            TraversalController::runBfs,
            "/api/v1/graphs/{1}/bfs",
            drogon::Post
        );
        //  "filters::AuthFilter"

        ADD_METHOD_TO(
            TraversalController::getNeighbors,
            "/api/v1/graphs/{1}/vertices/{2}/neighbors",
            drogon::Get,
        );
        // "filters::AuthFilter"

        ADD_METHOD_TO(
            TraversalController::getNeighborsBatch,
            "/api/v1/graphs/{1}/neighbors/batch",
            drogon::Post,
        );
        // "filters::AuthFilter"

        METHOD_LIST_END

        void runBfs(
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            const std::string& graphId
        ) const {
            try {
                std::cout << "BFS query" << std::endl;
                const auto& body = http::parse::jsonBody(req);
                const auto bfsRequest = dto::BfsRequest::fromJson(body);

                auto* services = drogon::app().getPlugin<::services::ServicesPlugin>();
                auto result = services->traversalService().runBfs(graphId, bfsRequest);
                nlohmann::json jsonResult = result;


                nlohmann::json response = {
                    {"graphId", graphId},
                    {"sourceVertexId", bfsRequest.sourceVertexId},
                    {"maxDepth", bfsRequest.maxDepth},
                    {"result", result}
                };

                callback(http::jsonOk(std::move(response)));
            } catch (const std::exception& e) {
                callback(http::mapExceptionToResponse(e));
            } catch (...) {
                callback(http::mapUnknownExceptionToResponse());
            }
        }

        void getNeighbors(
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            std::string graphId,
            std::string vertexId
        ) const {}

        void getNeighborsBatch(
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            std::string graphId
        ) const {}
    };

}

#endif //CPGDB_TRAVERSALCONTROLLER_HPP
