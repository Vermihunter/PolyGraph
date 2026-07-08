//
// Created by akos on 7. 7. 2026.
//

#include "server/services/ServicesPlugin.hpp"

#include <optional>
#include <stdexcept>
#include <string>

#include "cli/GraphRuntimeBuilder.hpp"

namespace services {
    namespace {

        std::string required_string(const Json::Value& config, const char* key) {
            if (!config.isMember(key) || !config[key].isString() || config[key].asString().empty()) {
                throw std::runtime_error(std::string("Missing required ServicesPlugin config field: ") + key);
            }

            return config[key].asString();
        }

        std::optional<std::string> optional_string(const Json::Value& config, const char* key) {
            if (!config.isMember(key) || !config[key].isString() || config[key].asString().empty()) {
                return std::nullopt;
            }

            return config[key].asString();
        }

    } // namespace

    void ServicesPlugin::initAndStart(const Json::Value& config) {
        cpgdb::cli::GraphRuntimeConfig runtime_config{
            .logical_graph_schema_path = required_string(config, "logical_graph_schema"),
            .data_path = required_string(config, "data"),
            .backend_config_path = optional_string(config, "backend_config"),
            .graph_version = config.get("graph_version", Json::UInt64{1}).asUInt64(),
            .verbose = config.get("verbose", false).asBool()
        };

        cpgdb::cli::GraphRuntimeBuilder builder;
        ctx_ = builder.build(runtime_config);

        traversalService_ = std::make_unique<TraversalService>(*ctx_);
    }

    void ServicesPlugin::shutdown() {
        traversalService_.reset();
        ctx_.reset();
    }

}