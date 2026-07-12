/**
 * @file include/server/services/ServicesPlugin.hpp
 * @brief Declarations for services plugin in the server frontend module.
 */

#ifndef CPGDB_SERVER_SERVICES_SERVICES_PLUGIN_HPP
#define CPGDB_SERVER_SERVICES_SERVICES_PLUGIN_HPP

#include "infrastructure/DatabaseContext.hpp"
#include "server/services/TraversalService.hpp"

#include <drogon/plugins/Plugin.h>
#include <memory>

namespace cpgdb::services {

class ServicesPlugin final : public drogon::Plugin<ServicesPlugin> {
  public:
    void initAndStart(const Json::Value& config) override;
    void shutdown() override;

    [[nodiscard]] TraversalService& traversalService();
    [[nodiscard]] const DatabaseContext& dbContext() const;

  private:
    std::shared_ptr<DatabaseContext> context_;
    std::unique_ptr<TraversalService> traversal_service_;
};

} // namespace cpgdb::services

#endif // CPGDB_SERVER_SERVICES_SERVICES_PLUGIN_HPP
