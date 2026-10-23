/**
 * @file include/observability/MetricsAccess.hpp
 * @brief Process-wide access to the configured metrics service.
 */

#pragma once

#include "events/EventLevel.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace polygraph::observability::metrics {
class MetricsService;
}

namespace polygraph::observability {

struct GpuProjectionMetrics {
    std::size_t vertices = 0;
    std::size_t input_edges = 0;
    std::size_t raw_adjacency_pairs = 0;
    std::size_t unique_adjacency_pairs = 0;
    std::size_t duplicate_adjacency_pairs_removed = 0;
    std::size_t host_id_mapping_required_bytes = 0;
    std::size_t host_id_mapping_allocated_bytes = 0;
    std::size_t device_vertex_input_bytes = 0;
    std::size_t device_edge_input_bytes = 0;
    double build_duration_seconds = 0.0;
    bool mirrored_for_undirected_expansion = false;
};

struct MMapCacheMetrics {
    std::size_t mapped_bytes = 0;
    std::size_t index_bytes = 0;
    std::size_t neighbor_bytes = 0;
    std::size_t vertices = 0;
    std::size_t relations = 0;
};

void install_metrics_service(const std::shared_ptr<metrics::MetricsService>& service);
void shutdown_metrics() noexcept;
[[nodiscard]] std::shared_ptr<metrics::MetricsService> metrics_service() noexcept;

[[nodiscard]] std::uint64_t begin_http_request(std::string_view route, std::string_view method) noexcept;
void finish_http_request(std::uint64_t request_id,
                         std::string_view route,
                         std::string_view method,
                         int status_code,
                         double duration_seconds) noexcept;
void observe_http_request(std::string_view route,
                          std::string_view method,
                          int status_code,
                          double duration_seconds) noexcept;
void observe_event(EventLevel level) noexcept;
void set_gpu_projection_metrics(const GpuProjectionMetrics& snapshot) noexcept;
void set_mmap_cache_metrics(const MMapCacheMetrics& snapshot) noexcept;

} // namespace polygraph::observability
