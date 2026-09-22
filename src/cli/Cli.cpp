/**
 * @file src/cli/Cli.cpp
 * @brief Shared observability command-line parsing.
 */
#include "cli/Cli.hpp"
#include "cli/DomainManifest.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace polygraph::cli {
    namespace {

        [[nodiscard]] std::filesystem::path resolve_project_path(std::filesystem::path path) {
#ifdef POLYGRAPH_PROJECT_ROOT
            if (path.is_relative()) {
                return std::filesystem::path{POLYGRAPH_PROJECT_ROOT} / path;
            }
#endif
            return path;
        }

        [[nodiscard]] std::string timestamped_event_name() {
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            std::ostringstream name;
            name << "polygraph-" << std::put_time(&local, "%Y%m%d-%H%M%S") << ".events.jsonl";
            return name.str();
        }

        void override_event_file_sink(observability::EventGatheringOptions &events, const std::filesystem::path &path) {
            for (auto &sink: events.sinks) {
                if (sink.type == observability::SinkType::BasicFile ||
                    sink.type == observability::SinkType::RotatingFile) {
                    sink.path = resolve_project_path(path);
                    sink.enabled = true;
                    return;
                }
            }

            observability::EventSinkOptions sink;
            sink.name = "cli-events-json";
            sink.type = observability::SinkType::RotatingFile;
            sink.format = observability::EventFormat::Json;
            sink.minimum_level = EventLevel::Debug;
            sink.flush_level = EventLevel::Warn;
            sink.enabled = true;
            sink.color = false;
            sink.path = resolve_project_path(path);
            events.sinks.push_back(std::move(sink));
        }

        void override_application_log_file(observability::LoggingOptions &logging, const std::filesystem::path &path) {
            const auto extension = path.extension().string();
            if (extension == ".jsonl" || extension == ".ndjson") {
                throw std::invalid_argument(
                        "Operational logs are text, not JSONL. Use --log-file <name>.log for logs and "
                        "--event-file <name>.events.jsonl for structured events.");
            }
            for (auto &sink: logging.sinks) {
                if (sink.type == observability::SinkType::BasicFile ||
                    sink.type == observability::SinkType::RotatingFile) {
                    sink.path = resolve_project_path(path);
                    sink.enabled = true;
                    return;
                }
            }

            observability::LogSinkOptions sink;
            sink.name = "cli-application-log";
            sink.type = observability::SinkType::RotatingFile;
            sink.minimum_level = observability::LogLevel::Debug;
            sink.flush_level = observability::LogLevel::Error;
            sink.enabled = true;
            sink.color = false;
            sink.path = resolve_project_path(path);
            logging.sinks.push_back(std::move(sink));
        }

    } // namespace

    void add_log_arguments(CommandLine &program) {
        program.add_value_option("observability-config", {"--observability-config"},
                                 "Compatibility override for domain.paths.observabilityConfig", std::string{});

        // Structured execution events are JSONL and have their own CLI namespace.
        program.add_value_option("event-dir", {"--event-dir"}, "Directory of the structured event JSONL file",
                                 std::string{});
        program.add_value_option("event-name", {"--event-name"},
                                 "Event JSONL filename; timestamped when only --event-dir is set", std::string{});
        program.add_value_option("event-file", {"--event-file"}, "Exact structured event JSONL path", std::string{});
        program.add_flag_option("no-event-log", {"--no-event-log", "--no-events"},
                                "Disable runtime event gathering/output");

        // Operational logging is independent from execution events.
        program.add_value_option("application-log-file", {"--application-log-file", "--log-file"},
                                 "Operational text log file (.log recommended); logs also go to stdout", std::string{});
        program.add_flag_option("no-logging", {"--no-logging"}, "Disable operational application logging");
    }

    observability::ObservabilityConfig parse_observability_config(const CommandLine &args) {
        std::filesystem::path config_path = args.string_value("observability-config");
        if (config_path.empty()) {
            const auto &domain_path = args.string_value("domain");
            config_path = domain_path.empty() ? resolve_project_path("config/observability/observability.json")
                                              : load_domain_manifest(domain_path).observability_config_path;
        }
        auto config = observability::load_observability_config(config_path);

        if (args.bool_value("no-event-log")) {
            config.events.enabled = false;
        } else {
            const auto &explicit_file = args.string_value("event-file");
            if (!explicit_file.empty()) {
                override_event_file_sink(config.events, explicit_file);
            } else {
                const auto &directory = args.string_value("event-dir");
                const auto &name = args.string_value("event-name");
                if (!directory.empty() || !name.empty()) {
                    const auto file_name = name.empty() ? timestamped_event_name() : name;
                    override_event_file_sink(config.events, (directory.empty() ? std::filesystem::path{"logs"}
                                                                               : std::filesystem::path{directory}) /
                                                                    file_name);
                }
            }
        }

        if (args.bool_value("no-logging")) {
            config.logging.enabled = false;
        } else if (const auto &application_log = args.string_value("application-log-file"); !application_log.empty()) {
            override_application_log_file(config.logging, application_log);
        }

        return config;
    }

    observability::EventGatheringOptions parse_event_options(const CommandLine &args) {
        return parse_observability_config(args).events;
    }

    observability::LoggingOptions parse_log_options(const CommandLine &args) {
        return parse_observability_config(args).logging;
    }

} // namespace polygraph::cli
