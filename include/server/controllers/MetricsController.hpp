/**
 * @file include/server/controllers/MetricsController.hpp
 * @brief Prometheus pull endpoint.
 */

#pragma once

#include <drogon/drogon.h>

#include <functional>

namespace polygraph::http {

class MetricsController final : public drogon::HttpController<MetricsController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MetricsController::metrics, "/metrics", drogon::Get);
    METHOD_LIST_END

    static void metrics(const drogon::HttpRequestPtr& request,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace polygraph::http
