/**
 * @file include/observability/metrics/MetricsService.hpp
 * @brief Prometheus registry, pull-time collectors, and application metrics.
 */

#pragma once

#include "events/EventLevel.hpp"
#include "observability/ObservabilityConfig.hpp"
#include "observability/MetricsAccess.hpp"
#include "observability/metrics/MetricTypes.hpp"
#include "observability/metrics/collectors/IMetricsCollector.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace polygraph::observability::metrics {

class MetricsService {
  public:
    explicit MetricsService(MetricsOptions options);

    [[nodiscard]] bool enabled() const noexcept { return options_.enabled; }
    [[nodiscard]] const std::string& endpoint() const noexcept { return options_.endpoint; }
    [[nodiscard]] bool http_metrics_enabled() const noexcept {
        return options_.enabled && options_.collect_http_metrics;
    }
    [[nodiscard]] bool event_metrics_enabled() const noexcept {
        return options_.enabled && options_.collect_event_metrics;
    }

    [[nodiscard]] std::string render();
    [[nodiscard]] std::uint64_t begin_http(std::string_view route, std::string_view method);
    void finish_http(std::uint64_t request_id,
                     std::string_view route,
                     std::string_view method,
                     int status_code,
                     double duration_seconds);
    void observe_http(std::string_view route,
                      std::string_view method,
                      int status_code,
                      double duration_seconds);
    void observe_event(EventLevel level);
    void set_gpu_projection(const observability::GpuProjectionMetrics& snapshot);
    void set_mmap_cache(const observability::MMapCacheMetrics& snapshot);

  private:
    struct CollectorState {
        std::unique_ptr<IMetricsCollector> collector;
        MetricsCollectorOptions options;
        std::chrono::steady_clock::time_point last_collection{};
        MetricValues cached_values;
        bool is_available = false;
        bool has_collected = false;
        prometheus::Gauge* available = nullptr;
        prometheus::Counter* errors = nullptr;
        prometheus::Histogram* duration = nullptr;
    };

    struct HttpSeries {
        prometheus::Counter* requests = nullptr;
        prometheus::Histogram* duration = nullptr;
    };

    struct ActiveHttpSeries {
        prometheus::Gauge* in_progress = nullptr;
        prometheus::Gauge* oldest_seconds = nullptr;
        std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> requests;
    };

    void register_collectors();
    void register_application_metrics();
    void collect_state_metrics();
    void update_active_http_metrics();

    [[nodiscard]] std::string metric_name(std::string_view collector,
                                          std::string_view metric) const;
    [[nodiscard]] HttpSeries& http_series(std::string_view route,
                                          std::string_view method,
                                          std::string_view status_class);

    MetricsOptions options_;
    std::shared_ptr<prometheus::Registry> registry_;
    std::vector<CollectorState> collectors_;
    std::unordered_map<std::string, prometheus::Gauge*> state_gauges_;

    prometheus::Family<prometheus::Gauge>* collector_available_family_ = nullptr;
    prometheus::Family<prometheus::Counter>* collector_errors_family_ = nullptr;
    prometheus::Family<prometheus::Histogram>* collector_duration_family_ = nullptr;
    prometheus::Family<prometheus::Counter>* http_requests_family_ = nullptr;
    prometheus::Family<prometheus::Histogram>* http_duration_family_ = nullptr;
    prometheus::Family<prometheus::Gauge>* http_in_progress_family_ = nullptr;
    prometheus::Family<prometheus::Gauge>* http_oldest_in_progress_family_ = nullptr;
    prometheus::Family<prometheus::Counter>* event_family_ = nullptr;
    prometheus::Gauge* gpu_projection_vertices_ = nullptr;
    prometheus::Gauge* gpu_projection_input_edges_ = nullptr;
    prometheus::Gauge* gpu_projection_raw_adjacency_pairs_ = nullptr;
    prometheus::Gauge* gpu_projection_unique_adjacency_pairs_ = nullptr;
    prometheus::Gauge* gpu_projection_duplicate_pairs_removed_ = nullptr;
    prometheus::Gauge* gpu_projection_host_id_mapping_required_bytes_ = nullptr;
    prometheus::Gauge* gpu_projection_host_id_mapping_allocated_bytes_ = nullptr;
    prometheus::Gauge* gpu_projection_device_vertex_input_bytes_ = nullptr;
    prometheus::Gauge* gpu_projection_device_edge_input_bytes_ = nullptr;
    prometheus::Gauge* gpu_projection_build_duration_seconds_ = nullptr;
    prometheus::Gauge* gpu_projection_mirrored_ = nullptr;
    prometheus::Gauge* mmap_cache_mapped_bytes_ = nullptr;
    prometheus::Gauge* mmap_cache_index_bytes_ = nullptr;
    prometheus::Gauge* mmap_cache_neighbor_bytes_ = nullptr;
    prometheus::Gauge* mmap_cache_vertices_ = nullptr;
    prometheus::Gauge* mmap_cache_relations_ = nullptr;

    std::unordered_map<std::string, HttpSeries> http_series_;
    std::unordered_map<std::string, ActiveHttpSeries> active_http_series_;
    std::uint64_t next_http_request_id_ = 1;
    std::unordered_map<int, prometheus::Counter*> event_series_;
    std::mutex scrape_mutex_;
    std::mutex application_mutex_;
};

} // namespace polygraph::observability::metrics
