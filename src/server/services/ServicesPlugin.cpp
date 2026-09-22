/**
 * @file src/server/services/ServicesPlugin.cpp
 * @brief Runtime and observability lifecycle for the Drogon frontend.
 */

#include "server/services/ServicesPlugin.hpp"

#include "cli/DomainManifest.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "events/EventSetup.hpp"
#include "events/Events.hpp"
#include "logging/Log.hpp"
#include "logging/LoggingSetup.hpp"
#include "observability/MetricsAccess.hpp"
#include "observability/ObservabilityConfig.hpp"
#ifdef POLYGRAPH_ENABLE_METRICS
#include "observability/metrics/MetricsService.hpp"
#endif

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace polygraph::services {
    namespace {

        [[nodiscard]] std::string required_string(const Json::Value &config, const char *key) {
            if (!config.isMember(key) || !config[key].isString() || config[key].asString().empty()) {
                throw std::runtime_error(std::string("Missing required ServicesPlugin config field: ") + key);
            }
            return config[key].asString();
        }

        [[nodiscard]] std::optional<std::string> optional_string(const Json::Value &config, const char *key) {
            if (!config.isMember(key) || !config[key].isString() || config[key].asString().empty()) {
                return std::nullopt;
            }
            return config[key].asString();
        }

    } // namespace

    void ServicesPlugin::initAndStart(const Json::Value &config) {
        cli::GraphRuntimeConfig runtime_config;
        std::filesystem::path observability_path;
        std::string domain_path;

        if (const auto configured_domain = optional_string(config, "domain")) {
            const auto domain = cli::load_domain_manifest(*configured_domain);
            domain_path = domain.manifest_path.string();
            observability_path =
                    optional_string(config, "observability_config").value_or(domain.observability_config_path.string());
            runtime_config = cli::GraphRuntimeConfig{
                    .logical_graph_schema_path = domain.logical_graph_schema_path.string(),
                    .data_path = domain.data_path.string(),
                    .backend_config_path = domain.backend_config_path.string(),
                    .native_id_map_path = domain.native_id_map_path
                                                  ? std::optional<std::string>{domain.native_id_map_path->string()}
                                                  : std::nullopt,
                    .graph_version = domain.runtime.graph_version,
                    .verbose = domain.runtime.verbose,
            };
        } else {
            // Compatibility path for older Drogon configurations.
            observability_path =
                    optional_string(config, "observability_config").value_or("config/observability/observability.json");
            runtime_config = cli::GraphRuntimeConfig{
                    .logical_graph_schema_path = required_string(config, "logical_graph_schema"),
                    .data_path = required_string(config, "data"),
                    .backend_config_path = optional_string(config, "backend_config"),
                    .native_id_map_path = optional_string(config, "native_id_map"),
                    .graph_version = config.get("graph_version", Json::UInt64{1}).asUInt64(),
                    .verbose = config.get("verbose", false).asBool(),
            };
        }

        if (const auto configured_cache_plan = optional_string(config, "cache_plan")) {
            runtime_config.cache_plan_path = *configured_cache_plan;
        }

        const auto observability_config = observability::load_observability_config(observability_path);
        logging::install_logger(observability_config.logging);
        events::install_collector(observability_config.events);

#ifdef POLYGRAPH_ENABLE_METRICS
        metrics_service_ = std::make_shared<observability::metrics::MetricsService>(observability_config.metrics);
        observability::install_metrics_service(metrics_service_);
#endif

        LOG_INFO("Starting PolyGraph server");
        EVENT_INFO("server.starting", {"domain", domain_path}, {"observability_config", observability_path.string()},
                   {"cache_plan", runtime_config.cache_plan_path.value_or("")});

        try {
            context_ = cli::GraphRuntimeBuilder::build(runtime_config);
            traversal_service_ = std::make_unique<TraversalService>(*context_);
            EVENT_INFO("server.started", {"metrics_endpoint", observability_config.metrics.endpoint});
            LOG_INFO("PolyGraph server started");
        } catch (...) {
            observability::shutdown_metrics();
            metrics_service_.reset();
            events::shutdown();
            logging::shutdown();
            throw;
        }
    }

    void ServicesPlugin::shutdown() {
        EVENT_INFO("server.stopping");
        LOG_INFO("Stopping PolyGraph server");
        traversal_service_.reset();
        context_.reset();
        observability::shutdown_metrics();
        metrics_service_.reset();
        events::shutdown();
        logging::shutdown();
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

} // namespace polygraph::services
