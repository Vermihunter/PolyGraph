#include "observability/MetricsAccess.hpp"

#ifdef POLYGRAPH_ENABLE_METRICS
#include "observability/metrics/MetricsService.hpp"

#include <atomic>
#include <memory>
#include <utility>
#endif

namespace polygraph::observability {

#ifdef POLYGRAPH_ENABLE_METRICS

    namespace {

        std::shared_ptr<metrics::MetricsService> global_metrics;

        std::atomic_bool global_http_metrics_enabled{false};
        std::atomic_bool global_event_metrics_enabled{false};

    } // namespace

    void install_metrics_service(const std::shared_ptr<metrics::MetricsService> &service) {
        const bool http_enabled = service && service->http_metrics_enabled();

        const bool event_enabled = service && service->event_metrics_enabled();

        std::atomic_store_explicit(&global_metrics, service, std::memory_order_release);

        global_http_metrics_enabled.store(http_enabled, std::memory_order_release);

        global_event_metrics_enabled.store(event_enabled, std::memory_order_release);
    }

    void shutdown_metrics() noexcept {
        global_http_metrics_enabled.store(false, std::memory_order_release);

        global_event_metrics_enabled.store(false, std::memory_order_release);

        std::atomic_store_explicit(&global_metrics, std::shared_ptr<metrics::MetricsService>{},
                                   std::memory_order_release);
    }

    std::shared_ptr<metrics::MetricsService> metrics_service() noexcept {
        return std::atomic_load_explicit(&global_metrics, std::memory_order_acquire);
    }

    std::uint64_t begin_http_request(const std::string_view route, const std::string_view method) noexcept {
        if (!global_http_metrics_enabled.load(std::memory_order_relaxed)) {
            return 0;
        }
        try {
            if (auto service = metrics_service()) {
                return service->begin_http(route, method);
            }
        } catch (...) {
        }
        return 0;
    }

    void finish_http_request(const std::uint64_t request_id, const std::string_view route,
                             const std::string_view method, const int status_code,
                             const double duration_seconds) noexcept {
        if (!global_http_metrics_enabled.load(std::memory_order_relaxed)) {
            return;
        }
        try {
            if (auto service = metrics_service()) {
                service->finish_http(request_id, route, method, status_code, duration_seconds);
            }
        } catch (...) {
        }
    }

    void observe_http_request(const std::string_view route, const std::string_view method, const int status_code,
                              const double duration_seconds) noexcept {
        if (!global_http_metrics_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        try {
            if (auto service = metrics_service()) {
                service->observe_http(route, method, status_code, duration_seconds);
            }
        } catch (...) {
            // Metrics must never affect request processing.
        }
    }

    void observe_event(const EventLevel level) noexcept {
        if (!global_event_metrics_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        try {
            if (auto service = metrics_service()) {
                service->observe_event(level);
            }
        } catch (...) {
            // Metrics must never affect event processing.
        }
    }

    void set_gpu_projection_metrics(const GpuProjectionMetrics &snapshot) noexcept {
        try {
            if (auto service = metrics_service()) {
                service->set_gpu_projection(snapshot);
            }
        } catch (...) {
            // Metrics must never affect graph construction.
        }
    }

    void set_mmap_cache_metrics(const MMapCacheMetrics &snapshot) noexcept {
        try {
            if (auto service = metrics_service()) {
                service->set_mmap_cache(snapshot);
            }
        } catch (...) {
            // Metrics must never affect cache construction.
        }
    }

#else

    void install_metrics_service(const std::shared_ptr<metrics::MetricsService> &) {}

    void shutdown_metrics() noexcept {}

    std::shared_ptr<metrics::MetricsService> metrics_service() noexcept { return {}; }

    std::uint64_t begin_http_request(std::string_view, std::string_view) noexcept { return 0; }

    void finish_http_request(std::uint64_t, std::string_view, std::string_view, int, double) noexcept {}

    void observe_http_request(std::string_view, std::string_view, int, double) noexcept {}

    void observe_event(EventLevel) noexcept {}

    void set_gpu_projection_metrics(const GpuProjectionMetrics &) noexcept {}

    void set_mmap_cache_metrics(const MMapCacheMetrics &) noexcept {}

#endif

} // namespace polygraph::observability
