/**
 * @file src/observability/ObservabilityConfig.cpp
 * @brief Parsing and validation of event, logging and metrics configuration.
 */
#include "observability/ObservabilityConfig.hpp"

#include "utilities/json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace polygraph::observability {
    namespace {

        [[nodiscard]] std::string lowercase(std::string value) {
            std::ranges::transform(value, value.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        [[nodiscard]] std::filesystem::path project_root() {
#ifdef POLYGRAPH_PROJECT_ROOT
            return std::filesystem::path{POLYGRAPH_PROJECT_ROOT};
#else
            return std::filesystem::current_path();
#endif
        }

        [[nodiscard]] std::filesystem::path resolve_path(std::filesystem::path path) {
            return path.is_absolute() ? std::move(path) : project_root() / path;
        }

        [[nodiscard]] std::vector<std::string> string_array(const json &parent, const std::string_view key) {
            const auto iterator = parent.find(std::string{key});
            if (iterator == parent.end()) {
                return {};
            }
            if (!iterator->is_array()) {
                throw std::runtime_error(std::string{key} + " must be an array of strings");
            }

            std::vector<std::string> values;
            values.reserve(iterator->size());
            for (const auto &value: *iterator) {
                if (!value.is_string()) {
                    throw std::runtime_error(std::string{key} + " must contain only strings");
                }
                values.push_back(value.get<std::string>());
            }
            return values;
        }

        [[nodiscard]] EventSelection parse_event_selection(const json &value) {
            EventSelection selection;
            selection.include_prefixes = string_array(value, "include_prefixes");
            selection.exclude_prefixes = string_array(value, "exclude_prefixes");
            return selection;
        }

        [[nodiscard]] SinkType parse_sink_type(const std::string &raw) {
            const auto value = lowercase(raw);
            if (value == "stdout")
                return SinkType::Stdout;
            if (value == "stderr")
                return SinkType::Stderr;
            if (value == "file" || value == "basic_file")
                return SinkType::BasicFile;
            if (value == "rotating_file")
                return SinkType::RotatingFile;
            throw std::runtime_error("Unknown sink type: " + raw);
        }

        [[nodiscard]] EventFormat parse_event_format(const std::string &raw) {
            const auto value = lowercase(raw);
            if (value == "text")
                return EventFormat::Text;
            if (value == "json" || value == "jsonl")
                return EventFormat::Json;
            throw std::runtime_error("Unknown event format: " + raw);
        }

        [[nodiscard]] AsyncOverflowPolicy parse_overflow_policy(const std::string &raw) {
            const auto value = lowercase(raw);
            if (value == "block")
                return AsyncOverflowPolicy::Block;
            if (value == "overrun_oldest" || value == "drop_oldest")
                return AsyncOverflowPolicy::OverrunOldest;
            throw std::runtime_error("Unknown async overflow policy: " + raw);
        }

        void validate_file_sink(const SinkType type, const std::filesystem::path &path, std::size_t max_size_bytes,
                                std::size_t max_files, const std::string &name) {
            if ((type == SinkType::BasicFile || type == SinkType::RotatingFile) && path.empty()) {
                throw std::runtime_error("File sink '" + name + "' requires path");
            }
            if (type == SinkType::RotatingFile && (max_size_bytes == 0 || max_files == 0)) {
                throw std::runtime_error("Rotating sink '" + name + "' requires positive max_size_mb and max_files");
            }
        }

        [[nodiscard]] EventSinkOptions parse_event_sink(const json &value, const std::size_t index) {
            if (!value.is_object()) {
                throw std::runtime_error("events.sinks entries must be objects");
            }

            EventSinkOptions result;
            result.name = value.value("name", "event-sink-" + std::to_string(index));
            result.enabled = value.value("enabled", true);
            result.type = parse_sink_type(value.value("type", "rotating_file"));
            result.format = parse_event_format(value.value("format", "json"));
            result.minimum_level = parse_event_level(value.value("minimum_level", "debug"));
            result.flush_level = parse_event_level(value.value("flush_level", "warn"));
            result.color = value.value("color", false);
            result.truncate = value.value("truncate", false);
            result.rotate_on_open = value.value("rotate_on_open", false);
            result.pattern = value.value("pattern", std::string{});
            result.max_size_bytes = value.value("max_size_mb", 256U) * 1024U * 1024U;
            result.max_files = value.value("max_files", 8U);
            result.selection = parse_event_selection(value.value("selection", value.value("events", json::object())));

            if (value.contains("path")) {
                if (!value.at("path").is_string()) {
                    throw std::runtime_error("event sink path must be a string");
                }
                result.path = resolve_path(value.at("path").get<std::string>());
            }

            validate_file_sink(result.type, result.path, result.max_size_bytes, result.max_files, result.name);
            return result;
        }

        [[nodiscard]] LogSinkOptions parse_log_sink(const json &value, const std::size_t index) {
            if (!value.is_object()) {
                throw std::runtime_error("logging.sinks entries must be objects");
            }

            LogSinkOptions result;
            result.name = value.value("name", "log-sink-" + std::to_string(index));
            result.enabled = value.value("enabled", true);
            result.type = parse_sink_type(value.value("type", "stdout"));
            result.minimum_level = parse_log_level(value.value("minimum_level", "info"));
            result.flush_level = parse_log_level(value.value("flush_level", "error"));
            result.color = value.value("color", result.type == SinkType::Stdout || result.type == SinkType::Stderr);
            result.truncate = value.value("truncate", false);
            result.rotate_on_open = value.value("rotate_on_open", false);
            result.pattern = value.value("pattern", std::string{});
            result.max_size_bytes = value.value("max_size_mb", 64U) * 1024U * 1024U;
            result.max_files = value.value("max_files", 5U);

            if (value.contains("path")) {
                if (!value.at("path").is_string()) {
                    throw std::runtime_error("logging sink path must be a string");
                }
                result.path = resolve_path(value.at("path").get<std::string>());
            }

            validate_file_sink(result.type, result.path, result.max_size_bytes, result.max_files, result.name);
            return result;
        }

        [[nodiscard]] EventGatheringOptions parse_events(const json &root) {
            EventGatheringOptions result = default_observability_config().events;
            if (!root.contains("events")) {
                return result;
            }

            const auto &value = root.at("events");
            if (!value.is_object()) {
                throw std::runtime_error("events must be an object");
            }

            result.enabled = value.value("enabled", result.enabled);
            result.service = value.value("service", result.service);
            result.minimum_level = parse_event_level(value.value("minimum_level", to_string(result.minimum_level)));
            result.selection = parse_event_selection(value.value("selection", json::object()));
            result.asynchronous = value.value("asynchronous", result.asynchronous);
            result.queue_size = value.value("queue_size", result.queue_size);
            result.worker_threads = value.value("worker_threads", result.worker_threads);
            result.overflow_policy = parse_overflow_policy(value.value("overflow_policy", "overrun_oldest"));
            result.periodic_flush_ms = value.value("periodic_flush_ms", result.periodic_flush_ms);

            if (result.queue_size == 0 || result.worker_threads == 0) {
                throw std::runtime_error("events queue_size and worker_threads must be positive");
            }

            if (value.contains("sinks")) {
                if (!value.at("sinks").is_array()) {
                    throw std::runtime_error("events.sinks must be an array");
                }
                result.sinks.clear();
                std::unordered_set<std::string> names;
                std::size_t index = 0;
                for (const auto &sink: value.at("sinks")) {
                    auto parsed = parse_event_sink(sink, index++);
                    if (!names.insert(parsed.name).second) {
                        throw std::runtime_error("Duplicate event sink name: " + parsed.name);
                    }
                    result.sinks.push_back(std::move(parsed));
                }
            }
            return result;
        }

        [[nodiscard]] LoggingOptions parse_logging(const json &root) {
            LoggingOptions result = default_observability_config().logging;
            if (!root.contains("logging")) {
                return result;
            }

            const auto &value = root.at("logging");
            if (!value.is_object()) {
                throw std::runtime_error("logging must be an object");
            }

            result.enabled = value.value("enabled", result.enabled);
            result.logger_name = value.value("logger_name", result.logger_name);
            result.minimum_level = parse_log_level(value.value("minimum_level", to_string(result.minimum_level)));
            result.asynchronous = value.value("asynchronous", result.asynchronous);
            result.queue_size = value.value("queue_size", result.queue_size);
            result.worker_threads = value.value("worker_threads", result.worker_threads);
            result.overflow_policy = parse_overflow_policy(value.value("overflow_policy", "overrun_oldest"));
            result.periodic_flush_ms = value.value("periodic_flush_ms", result.periodic_flush_ms);

            if (result.queue_size == 0 || result.worker_threads == 0) {
                throw std::runtime_error("logging queue_size and worker_threads must be positive");
            }

            if (value.contains("sinks")) {
                if (!value.at("sinks").is_array()) {
                    throw std::runtime_error("logging.sinks must be an array");
                }
                result.sinks.clear();
                std::unordered_set<std::string> names;
                std::size_t index = 0;
                for (const auto &sink: value.at("sinks")) {
                    auto parsed = parse_log_sink(sink, index++);
                    if (!names.insert(parsed.name).second) {
                        throw std::runtime_error("Duplicate logging sink name: " + parsed.name);
                    }
                    result.sinks.push_back(std::move(parsed));
                }
            }
            return result;
        }

        void validate_collector_metrics(const std::string &collector, const MetricsCollectorOptions &options) {
            static const std::unordered_map<std::string, std::unordered_set<std::string>> known{
                    {"process",
                     {"virtual_memory_bytes", "resident_memory_bytes", "resident_anonymous_bytes",
                      "resident_file_bytes", "swap_bytes", "threads", "open_file_descriptors", "max_file_descriptors",
                      "cpu_user_seconds_total", "cpu_system_seconds_total", "cpu_seconds_total"}},
                    {"jemalloc",
                     {"allocated_bytes", "active_bytes", "resident_bytes", "mapped_bytes", "retained_bytes",
                      "metadata_bytes", "dirty_bytes", "muzzy_bytes"}},
                    {"glibc",
                     {"arena_bytes", "allocated_bytes", "free_bytes", "mmap_bytes", "mmap_regions",
                      "releasable_bytes"}},
                    {"gpu",
                     {"memory_total_bytes", "memory_used_bytes", "memory_free_bytes", "process_memory_bytes",
                      "utilization_percent", "memory_utilization_percent", "power_usage_watts", "power_limit_watts",
                      "temperature_celsius", "fan_speed_percent", "graphics_clock_hz", "sm_clock_hz", "memory_clock_hz",
                      "compute_processes"}},
            };

            const auto known_collector = known.find(collector);
            if (known_collector == known.end()) {
                throw std::runtime_error("Unknown metrics collector: " + collector);
            }

            std::unordered_set<std::string> configured;
            for (const auto &metric: options.metrics) {
                if (!known_collector->second.contains(metric)) {
                    std::string message = "Unknown metric '";
                    message += collector;
                    message += '.';
                    message += metric;
                    message += '\'';
                    throw std::runtime_error(message);
                }

                if (!configured.insert(metric).second) {
                    std::string message = "Duplicate metric '";
                    message += collector;
                    message += '.';
                    message += metric;
                    message += '\'';
                    throw std::runtime_error(message);
                }
            }
        }

        [[nodiscard]] bool valid_prometheus_namespace(const std::string_view value) {
            if (value.empty())
                return false;
            const auto first = value.front();
            if (!(std::isalpha(static_cast<unsigned char>(first)) || first == '_' || first == ':')) {
                return false;
            }
            return std::ranges::all_of(value.substr(1), [](const unsigned char character) {
                return std::isalnum(character) || character == '_' || character == ':';
            });
        }

        [[nodiscard]] MetricsOptions parse_metrics(const json &root) {
            MetricsOptions result = default_observability_config().metrics;
            if (!root.contains("metrics")) {
                return result;
            }

            const auto &value = root.at("metrics");
            if (!value.is_object()) {
                throw std::runtime_error("metrics must be an object");
            }

            result.enabled = value.value("enabled", result.enabled);
            result.endpoint = value.value("endpoint", result.endpoint);
            result.metric_namespace = value.value("namespace", result.metric_namespace);
            result.include_build_info = value.value("include_build_info", result.include_build_info);
            result.collect_http_metrics = value.value("collect_http_metrics", result.collect_http_metrics);
            result.collect_event_metrics = value.value("collect_event_metrics", result.collect_event_metrics);

            if (value.contains("http_duration_buckets_seconds")) {
                const auto &buckets = value.at("http_duration_buckets_seconds");
                if (!buckets.is_array() || buckets.empty()) {
                    throw std::runtime_error("http_duration_buckets_seconds must be a non-empty array");
                }
                result.http_duration.seconds_buckets = buckets.get<std::vector<double>>();
                const auto &values = result.http_duration.seconds_buckets;
                if (values.front() <= 0.0 ||
                    !std::ranges::all_of(values, [](const double bucket) { return bucket > 0.0; }) ||
                    std::adjacent_find(values.begin(), values.end(), [](const double left, const double right) {
                        return left >= right;
                    }) != values.end()) {
                    throw std::runtime_error("http_duration_buckets_seconds must be positive and strictly increasing");
                }
            }

            if (value.contains("collectors")) {
                if (!value.at("collectors").is_object()) {
                    throw std::runtime_error("metrics.collectors must be an object");
                }
                result.collectors.clear();
                for (const auto &[name, collector]: value.at("collectors").items()) {
                    if (!collector.is_object()) {
                        throw std::runtime_error("metrics collector '" + name + "' must be an object");
                    }
                    MetricsCollectorOptions options;
                    options.enabled = collector.value("enabled", false);
                    options.cache_ttl_ms = collector.value("cache_ttl_ms", 250U);
                    options.device_index = collector.value("device_index", 0U);
                    if (collector.contains("device_indices")) {
                        if (!collector.at("device_indices").is_array()) {
                            throw std::runtime_error("metrics collector '" + name +
                                                     "'.device_indices must be an array");
                        }
                        options.device_indices = collector.at("device_indices").get<std::vector<unsigned int>>();
                    }
                    options.metrics = string_array(collector, "metrics");
                    validate_collector_metrics(name, options);
                    result.collectors.emplace(name, std::move(options));
                }
            }

            if (result.endpoint != "/metrics") {
                throw std::runtime_error("metrics.endpoint must be '/metrics' because Drogon controller routes are "
                                         "compile-time registered");
            }
            if (!valid_prometheus_namespace(result.metric_namespace)) {
                throw std::runtime_error("metrics.namespace is not a valid Prometheus metric prefix");
            }
            return result;
        }

    } // namespace

    ObservabilityConfig default_observability_config() {
        ObservabilityConfig result;

        EventSinkOptions event_file;
        event_file.name = "events-json";
        event_file.type = SinkType::RotatingFile;
        event_file.format = EventFormat::Json;
        event_file.minimum_level = EventLevel::Debug;
        event_file.flush_level = EventLevel::Warn;
        event_file.color = false;
        event_file.path = project_root() / "logs/polygraph.events.jsonl";
        result.events.sinks = {std::move(event_file)};

        LogSinkOptions console;
        console.name = "console";
        console.type = SinkType::Stdout;
        console.minimum_level = LogLevel::Info;
        console.flush_level = LogLevel::Error;
        console.color = true;
        console.pattern = "%Y-%m-%dT%H:%M:%S.%e%z [%l] %v";

        LogSinkOptions log_file;
        log_file.name = "application-log";
        log_file.type = SinkType::RotatingFile;
        log_file.minimum_level = LogLevel::Debug;
        log_file.flush_level = LogLevel::Error;
        log_file.color = false;
        log_file.path = project_root() / "logs/polygraph.log";
        log_file.pattern = "%Y-%m-%dT%H:%M:%S.%e%z [%l] [thread %t] %v";
        result.logging.sinks = {std::move(console), std::move(log_file)};

        result.metrics.collectors = {
                {"process", MetricsCollectorOptions{.enabled = true,
                                                    .cache_ttl_ms = 250,
                                                    .metrics = {"virtual_memory_bytes", "resident_memory_bytes",
                                                                "resident_anonymous_bytes", "resident_file_bytes",
                                                                "swap_bytes", "threads", "open_file_descriptors",
                                                                "max_file_descriptors", "cpu_user_seconds_total",
                                                                "cpu_system_seconds_total", "cpu_seconds_total"}}},
                {"jemalloc", MetricsCollectorOptions{.enabled = true,
                                                     .cache_ttl_ms = 250,
                                                     .metrics = {"allocated_bytes", "active_bytes", "resident_bytes",
                                                                 "mapped_bytes", "retained_bytes", "metadata_bytes",
                                                                 "dirty_bytes", "muzzy_bytes"}}},
                {"glibc", MetricsCollectorOptions{.enabled = true,
                                                  .cache_ttl_ms = 250,
                                                  .metrics = {"arena_bytes", "allocated_bytes", "free_bytes",
                                                              "mmap_bytes", "mmap_regions", "releasable_bytes"}}},
                {"gpu",
                 MetricsCollectorOptions{.enabled = true,
                                         .cache_ttl_ms = 500,
                                         .device_index = 0,
                                         .device_indices = {0},
                                         .metrics = {"memory_total_bytes", "memory_used_bytes", "memory_free_bytes",
                                                     "process_memory_bytes", "utilization_percent",
                                                     "memory_utilization_percent", "power_usage_watts",
                                                     "power_limit_watts", "temperature_celsius", "fan_speed_percent",
                                                     "graphics_clock_hz", "sm_clock_hz", "memory_clock_hz",
                                                     "compute_processes"}}},
        };
        return result;
    }

    ObservabilityConfig load_observability_config(const std::filesystem::path &path) {
        if (path.empty()) {
            return default_observability_config();
        }

        const auto resolved = resolve_path(path);
        std::ifstream input(resolved);
        if (!input) {
            throw std::runtime_error("Could not open observability config: " + resolved.string());
        }

        const json root = json::parse(input);
        return ObservabilityConfig{
                .events = parse_events(root),
                .logging = parse_logging(root),
                .metrics = parse_metrics(root),
        };
    }

    EventLevel parse_event_level(const std::string &raw) {
        const auto value = lowercase(raw);
        if (value == "trace")
            return EventLevel::Trace;
        if (value == "debug")
            return EventLevel::Debug;
        if (value == "info")
            return EventLevel::Info;
        if (value == "warn" || value == "warning")
            return EventLevel::Warn;
        if (value == "error")
            return EventLevel::Error;
        if (value == "off")
            return EventLevel::Off;
        throw std::runtime_error("Unknown event level: " + raw);
    }

    LogLevel parse_log_level(const std::string &raw) {
        const auto value = lowercase(raw);
        if (value == "trace")
            return LogLevel::Trace;
        if (value == "debug")
            return LogLevel::Debug;
        if (value == "info")
            return LogLevel::Info;
        if (value == "warn" || value == "warning")
            return LogLevel::Warn;
        if (value == "error")
            return LogLevel::Error;
        if (value == "critical")
            return LogLevel::Critical;
        if (value == "off")
            return LogLevel::Off;
        throw std::runtime_error("Unknown log level: " + raw);
    }

    std::string to_string(const EventLevel value) {
        switch (value) {
            case EventLevel::Trace:
                return "trace";
            case EventLevel::Debug:
                return "debug";
            case EventLevel::Info:
                return "info";
            case EventLevel::Warn:
                return "warn";
            case EventLevel::Error:
                return "error";
            case EventLevel::Off:
                return "off";
        }
        return "off";
    }

    std::string to_string(const LogLevel value) {
        switch (value) {
            case LogLevel::Trace:
                return "trace";
            case LogLevel::Debug:
                return "debug";
            case LogLevel::Info:
                return "info";
            case LogLevel::Warn:
                return "warn";
            case LogLevel::Error:
                return "error";
            case LogLevel::Critical:
                return "critical";
            case LogLevel::Off:
                return "off";
        }
        return "off";
    }

} // namespace polygraph::observability
