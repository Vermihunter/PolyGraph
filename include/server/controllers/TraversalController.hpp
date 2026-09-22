/**
 * @file include/server/controllers/TraversalController.hpp
 * @brief Declarations for traversal controller in the server frontend module.
 */

#ifndef POLYGRAPH_SERVER_CONTROLLERS_TRAVERSAL_CONTROLLER_HPP
#define POLYGRAPH_SERVER_CONTROLLERS_TRAVERSAL_CONTROLLER_HPP

#include <drogon/drogon.h>
#include <functional>
#include <spdlog/spdlog.h>
#include <string>

namespace polygraph::http::api::v1 {

class TraversalController final : public drogon::HttpController<TraversalController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TraversalController::runBfs, "/api/v1/graphs/{1}/bfs", drogon::Post);
    ADD_METHOD_TO(TraversalController::getNeighbors, "/api/v1/graphs/{1}/neighbors", drogon::Post);
    METHOD_LIST_END

    static void runBfs(const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& source_vertex_uuid);

    static void getNeighbors(const drogon::HttpRequestPtr& request,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                      const std::string& source_vertex_uuid);
};

} // namespace polygraph::http::api::v1

#endif // POLYGRAPH_SERVER_CONTROLLERS_TRAVERSAL_CONTROLLER_HPP
