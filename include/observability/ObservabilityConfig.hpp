/**
 * @file include/observability/ObservabilityConfig.hpp
 * @brief Independent runtime configuration for events, operational logging and metrics.
 */
#pragma once

#include "events/EventLevel.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace polygraph::observability {

enum class SinkType {
    Stdout,
    Stderr,
    BasicFile,
    RotatingFile,
};

enum class EventFormat {
    Text,
    Json,
};

enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6,
};

enum class AsyncOverflowPolicy {
    Block,
    OverrunOldest,
};

struct EventSelection {
    std::vector<std::string> include_prefixes;
    std::vector<std::string> exclude_prefixes;
};

/** Output settings used only by the structured execution-event subsystem. */
struct EventSinkOptions {
    std::string name;
    SinkType type = SinkType::BasicFile;
    EventFormat format = EventFormat::Json;
    EventLevel minimum_level = EventLevel::Debug;
    EventLevel flush_level = EventLevel::Warn;
    bool enabled = true;
    bool color = false;
    bool truncate = false;
    bool rotate_on_open = false;
    std::filesystem::path path;
    std::string pattern;
    std::size_t max_size_bytes = 256U * 1024U * 1024U;
    std::size_t max_files = 8;
    EventSelection selection;
};

/** Runtime controls for event gathering. Compile-time removal is separate. */
struct EventGatheringOptions {
    bool enabled = true;
    std::string service = "polygraph";
    EventLevel minimum_level = EventLevel::Debug;
    EventSelection selection;
    bool asynchronous = true;
    std::size_t queue_size = 65536;
    std::size_t worker_threads = 1;
    AsyncOverflowPolicy overflow_policy = AsyncOverflowPolicy::OverrunOldest;
    std::uint64_t periodic_flush_ms = 1000;
    std::vector<EventSinkOptions> sinks;
};

/** Output settings used only by human/operational logging. */
struct LogSinkOptions {
    std::string name;
    SinkType type = SinkType::Stdout;
    LogLevel minimum_level = LogLevel::Info;
    LogLevel flush_level = LogLevel::Error;
    bool enabled = true;
    bool color = true;
    bool truncate = false;
    bool rotate_on_open = false;
    std::filesystem::path path;
    std::string pattern;
    std::size_t max_size_bytes = 64U * 1024U * 1024U;
    std::size_t max_files = 5;
};

struct LoggingOptions {
    bool enabled = true;
    std::string logger_name = "polygraph";
    LogLevel minimum_level = LogLevel::Info;
    bool asynchronous = true;
    std::size_t queue_size = 8192;
    std::size_t worker_threads = 1;
    AsyncOverflowPolicy overflow_policy = AsyncOverflowPolicy::OverrunOldest;
    std::uint64_t periodic_flush_ms = 1000;
    std::vector<LogSinkOptions> sinks;
};

struct MetricsCollectorOptions {
    bool enabled = false;
    std::uint64_t cache_ttl_ms = 250;
    unsigned int device_index = 0;
    std::vector<unsigned int> device_indices;
    std::vector<std::string> metrics;
};

struct HistogramOptions {
    std::vector<double> seconds_buckets{0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025,
                                        0.05,   0.1,   0.25,   0.5,   1.0,  2.5,
                                        5.0,    10.0,  30.0,   60.0,  120.0, 300.0,
                                        600.0,  900.0};
};

struct MetricsOptions {
    bool enabled = true;
    std::string endpoint = "/metrics";
    std::string metric_namespace = "polygraph";
    bool include_build_info = true;
    bool collect_http_metrics = true;
    bool collect_event_metrics = false;
    HistogramOptions http_duration;
    std::unordered_map<std::string, MetricsCollectorOptions> collectors;
};

struct ObservabilityConfig {
    EventGatheringOptions events;
    LoggingOptions logging;
    MetricsOptions metrics;
};

[[nodiscard]] ObservabilityConfig default_observability_config();
[[nodiscard]] ObservabilityConfig load_observability_config(const std::filesystem::path& path);

[[nodiscard]] EventLevel parse_event_level(const std::string& raw);
[[nodiscard]] LogLevel parse_log_level(const std::string& raw);
[[nodiscard]] std::string to_string(EventLevel value);
[[nodiscard]] std::string to_string(LogLevel value);

} // namespace polygraph::observability
