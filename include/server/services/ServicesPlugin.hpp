/**
 * @file include/server/services/ServicesPlugin.hpp
 * @brief Runtime, logging, and metrics lifecycle for the Drogon frontend.
 */

#pragma once

#include "infrastructure/DatabaseContext.hpp"
#include "server/services/TraversalService.hpp"

#include <drogon/plugins/Plugin.h>
#include <memory>

namespace polygraph::observability::metrics {
class MetricsService;
}

namespace polygraph::services {

class ServicesPlugin final : public drogon::Plugin<ServicesPlugin> {
  public:
    void initAndStart(const Json::Value& config) override;
    void shutdown() override;

    [[nodiscard]] TraversalService& traversalService();
    [[nodiscard]] const DatabaseContext& dbContext() const;

  private:
    std::shared_ptr<DatabaseContext> context_;
    std::unique_ptr<TraversalService> traversal_service_;
    std::shared_ptr<observability::metrics::MetricsService> metrics_service_;
};

} // namespace polygraph::services
