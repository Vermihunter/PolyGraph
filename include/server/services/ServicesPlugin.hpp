//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_SERVICESPLUGIN_HPP
#define CPGDB_SERVICESPLUGIN_HPP


#include "TraversalService.hpp"
#include "drogon/plugins/Plugin.h"
#include "infrastructure/DatabaseContext.hpp"

namespace services {

    class TraversalService;

    class ServicesPlugin final : public drogon::Plugin<ServicesPlugin> {
    public:
        void initAndStart(const Json::Value& config) override;
        void shutdown() override;

        TraversalService& traversalService() {
            return *traversalService_;
        }

        DatabaseContext& databaseContext() {
            return *ctx_;
        }

    private:
        std::shared_ptr<DatabaseContext> ctx_;
        std::unique_ptr<TraversalService> traversalService_;
    };

}

#endif //CPGDB_SERVICESPLUGIN_HPP
