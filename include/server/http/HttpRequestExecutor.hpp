/**
 * @file include/server/http/HttpRequestExecutor.hpp
 * @brief Exception mapping and low-overhead request metrics for synchronous handlers.
 */

#pragma once

#include "observability/MetricsAccess.hpp"
#include "server/http/ErrorMapper.hpp"

#include <chrono>
#include <drogon/HttpResponse.h>
#include <functional>
#include <string_view>
#include <utility>

namespace polygraph::http {

class HttpRequestExecutor {
  public:
    using ResponseCallback = std::function<void(const drogon::HttpResponsePtr&)>;

    template <typename Handler>
    static void executeRequest(ResponseCallback&& callback,
                               const std::string_view route,
                               const std::string_view method,
                               Handler&& handler) {
        auto response_callback = std::move(callback);
        const auto started = std::chrono::steady_clock::now();
        const auto request_metric_id = observability::begin_http_request(route, method);
        drogon::HttpResponsePtr response;

        try {
            response = std::forward<Handler>(handler)();
        } catch (const std::exception& error) {
            response = mapExceptionToResponse(error);
        } catch (...) {
            response = mapUnknownExceptionToResponse();
        }

        const double duration =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        observability::finish_http_request(
            request_metric_id, route, method, static_cast<int>(response->statusCode()), duration);
        response_callback(response);
    }
};

} // namespace polygraph::http
