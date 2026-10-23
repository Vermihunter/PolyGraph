#include "server/controllers/MetricsController.hpp"

#include "observability/MetricsAccess.hpp"
#include "observability/metrics/MetricsService.hpp"

namespace polygraph::http {

    void MetricsController::metrics(const drogon::HttpRequestPtr &,
                                    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const auto service = observability::metrics_service();
        if (!service || !service->enabled()) {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k404NotFound);
            response->setBody("Metrics are disabled\n");
            callback(response);
            return;
        }

        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
        response->addHeader("Cache-Control", "no-store");
        response->setBody(service->render());
        callback(response);
    }

} // namespace polygraph::http
