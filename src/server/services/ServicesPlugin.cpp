/**
 * @file src/server/services/ServicesPlugin.cpp
 * @brief Implementation for services plugin in the server frontend module.
 */

#include "server/services/ServicesPlugin.hpp"

#include "cli/GraphRuntimeBuilder.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace cpgdb::services {
    namespace {

        std::string required_string(const Json::Value &config, const char *key) {
            if (!config.isMember(key) || !config[key].isString() || config[key].asString().empty()) {
                throw std::runtime_error(std::string("Missing required ServicesPlugin config field: ") + key);
            }
            return config[key].asString();
        }

        std::optional<std::string> optional_string(const Json::Value &config, const char *key) {
            if (!config.isMember(key) || !config[key].isString() || config[key].asString().empty()) {
                return std::nullopt;
            }
            return config[key].asString();
        }

    } // namespace

    void ServicesPlugin::initAndStart(const Json::Value &config) {
        cli::GraphRuntimeConfig runtime_config{.logical_graph_schema_path =
                                                       required_string(config, "logical_graph_schema"),
                                               .data_path = required_string(config, "data"),
                                               .backend_config_path = optional_string(config, "backend_config"),
                                               .graph_version = config.get("graph_version", Json::UInt64{1}).asUInt64(),
                                               .verbose = config.get("verbose", false).asBool()};


        context_ = cli::GraphRuntimeBuilder::build(runtime_config);
        traversal_service_ = std::make_unique<TraversalService>(*context_);
    }

    void ServicesPlugin::shutdown() {
        traversal_service_.reset();
        context_.reset();
    }

    TraversalService &ServicesPlugin::traversalService() {
        if (!traversal_service_) {
            throw std::logic_error("ServicesPlugin has not been initialized");
        }
        return *traversal_service_;
    }

    const DatabaseContext &ServicesPlugin::dbContext() const {
        if (!context_) {
            throw std::logic_error("ServicesPlugin has not been initialized");
        }
        return *context_;
    }

} // namespace cpgdb::services
