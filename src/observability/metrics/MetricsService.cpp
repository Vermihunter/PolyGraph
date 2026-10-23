/**
 * @file src/observability/metrics/MetricsService.cpp
 * @brief prometheus-cpp metrics service implementation.
 */

#include "observability/metrics/MetricsService.hpp"

#include "observability/metrics/collectors/GlibcMetricsCollector.hpp"
#include "observability/metrics/collectors/GpuMetricsCollector.hpp"
#include "observability/metrics/collectors/JemallocMetricsCollector.hpp"
#include "observability/metrics/collectors/ProcessMetricsCollector.hpp"

#include <algorithm>
#include <chrono>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/text_serializer.h>
#include <stdexcept>
#include <utility>

namespace polygraph::observability::metrics {
    namespace {

        [[nodiscard]] std::string metric_help(const std::string_view collector, const std::string_view metric) {
            static const std::unordered_map<std::string, std::string> help{
                    {"process.virtual_memory_bytes", "Process virtual memory size in bytes."},
                    {"process.resident_memory_bytes", "Process resident memory size in bytes."},
                    {"process.resident_anonymous_bytes", "Anonymous resident process memory in bytes."},
                    {"process.resident_file_bytes", "File-backed resident process memory in bytes."},
                    {"process.swap_bytes", "Process swap usage in bytes."},
                    {"process.threads", "Number of process threads."},
                    {"process.open_file_descriptors", "Number of open process file descriptors."},
                    {"process.max_file_descriptors", "Configured process file descriptor limit."},
                    {"process.cpu_user_seconds_total", "Cumulative CPU seconds spent by PolyGraph in user mode."},
                    {"process.cpu_system_seconds_total", "Cumulative CPU seconds spent by PolyGraph in kernel mode."},
                    {"process.cpu_seconds_total", "Cumulative user plus kernel CPU seconds consumed by PolyGraph."},
                    {"jemalloc.allocated_bytes", "jemalloc bytes allocated to live application objects."},
                    {"jemalloc.active_bytes", "jemalloc active page bytes."},
                    {"jemalloc.resident_bytes", "jemalloc resident bytes."},
                    {"jemalloc.mapped_bytes", "jemalloc mapped bytes."},
                    {"jemalloc.retained_bytes", "jemalloc retained virtual memory bytes."},
                    {"jemalloc.metadata_bytes", "jemalloc allocator metadata bytes."},
                    {"jemalloc.dirty_bytes", "jemalloc dirty page bytes across all arenas."},
                    {"jemalloc.muzzy_bytes", "jemalloc muzzy page bytes across all arenas."},
                    {"glibc.arena_bytes", "glibc allocator non-mmap arena bytes."},
                    {"glibc.allocated_bytes", "glibc allocator in-use bytes reported by mallinfo2."},
                    {"glibc.free_bytes", "glibc allocator free bytes reported by mallinfo2."},
                    {"glibc.mmap_bytes", "glibc allocator mmap-backed bytes."},
                    {"glibc.mmap_regions", "glibc allocator mmap region count."},
                    {"glibc.releasable_bytes", "glibc allocator top-of-heap releasable bytes."},
                    {"gpu.memory_total_bytes", "Total framebuffer memory visible to NVML in bytes."},
                    {"gpu.memory_used_bytes", "Framebuffer memory currently used on the GPU in bytes."},
                    {"gpu.memory_free_bytes", "Framebuffer memory currently free on the GPU in bytes."},
                    {"gpu.process_memory_bytes", "Framebuffer memory attributed by NVML to this PolyGraph process."},
                    {"gpu.utilization_percent", "Recent GPU compute utilization percentage."},
                    {"gpu.memory_utilization_percent", "Recent GPU memory-controller utilization percentage."},
                    {"gpu.power_usage_watts", "Current GPU board power usage in watts."},
                    {"gpu.power_limit_watts", "Enforced GPU board power limit in watts."},
                    {"gpu.temperature_celsius", "Current GPU temperature in Celsius."},
                    {"gpu.fan_speed_percent", "Current GPU fan speed as a percentage of maximum."},
                    {"gpu.graphics_clock_hz", "Current GPU graphics clock in hertz."},
                    {"gpu.sm_clock_hz", "Current GPU streaming-multiprocessor clock in hertz."},
                    {"gpu.memory_clock_hz", "Current GPU memory clock in hertz."},
                    {"gpu.compute_processes", "Number of compute processes currently reported by NVML."},
            };

            const auto key = std::string(collector) + "." + std::string(metric);
            const auto iterator = help.find(key);
            return iterator == help.end() ? key : iterator->second;
        }

        [[nodiscard]] std::string status_class(const int status_code) {
            if (status_code < 100 || status_code > 599) {
                return "unknown";
            }
            return std::to_string(status_code / 100) + "xx";
        }

        [[nodiscard]] std::string http_key(const std::string_view route, const std::string_view method,
                                           const std::string_view status) {
            std::string result;
            result.reserve(route.size() + method.size() + status.size() + 2);
            result.append(route);
            result.push_back('\n');
            result.append(method);
            result.push_back('\n');
            result.append(status);
            return result;
        }

    } // namespace

    MetricsService::MetricsService(MetricsOptions options) :
        options_(std::move(options)), registry_(std::make_shared<prometheus::Registry>()) {
        if (!options_.enabled) {
            return;
        }
        register_collectors();
        register_application_metrics();

        if (options_.include_build_info) {
            auto &family = prometheus::BuildGauge()
                                   .Name(options_.metric_namespace + "_build_info")
                                   .Help("PolyGraph build and allocator information.")
                                   .Register(*registry_);
#ifdef POLYGRAPH_USE_JEMALLOC
            constexpr const char *allocator = "jemalloc";
#else
            constexpr const char *allocator = "system";
#endif
#ifdef POLYGRAPH_VERSION
            constexpr const char *version = POLYGRAPH_VERSION;
#else
            constexpr const char *version = "unknown";
#endif
            family.Add({{"version", version}, {"allocator", allocator}}).Set(1.0);
        }
    }

    std::string MetricsService::metric_name(const std::string_view collector, const std::string_view metric) const {
        return options_.metric_namespace + "_" + std::string(collector) + "_" + std::string(metric);
    }

    void MetricsService::register_collectors() {
        collector_available_family_ = &prometheus::BuildGauge()
                                               .Name(options_.metric_namespace + "_metrics_collector_available")
                                               .Help("Whether a configured pull-time metrics collector is available.")
                                               .Register(*registry_);
        collector_errors_family_ = &prometheus::BuildCounter()
                                            .Name(options_.metric_namespace + "_metrics_collection_errors_total")
                                            .Help("Pull-time metrics collector failures.")
                                            .Register(*registry_);
        collector_duration_family_ = &prometheus::BuildHistogram()
                                              .Name(options_.metric_namespace + "_metrics_collection_duration_seconds")
                                              .Help("Pull-time metrics collector duration.")
                                              .Register(*registry_);

        auto add_collector = [&](std::unique_ptr<IMetricsCollector> collector,
                                 const std::string &configuration_name = {}) {
            const std::string name(collector->name());
            const std::string config_name = configuration_name.empty() ? name : configuration_name;
            const auto configured = options_.collectors.find(config_name);
            if (configured == options_.collectors.end() || !configured->second.enabled ||
                configured->second.metrics.empty()) {
                return;
            }

            CollectorState state;
            state.collector = std::move(collector);
            state.options = configured->second;
            state.is_available = state.collector->available();
            state.available = &collector_available_family_->Add({{"collector", name}});
            state.available->Set(state.is_available ? 1.0 : 0.0);
            state.errors = &collector_errors_family_->Add({{"collector", name}});
            state.duration = &collector_duration_family_->Add(
                    {{"collector", name}}, prometheus::Histogram::BucketBoundaries{0.00005, 0.0001, 0.00025, 0.0005,
                                                                                   0.001, 0.0025, 0.005, 0.01, 0.05});
            if (!state.is_available) {
                collectors_.push_back(std::move(state));
                return;
            }

            for (const auto &metric: state.options.metrics) {
                auto &family = prometheus::BuildGauge()
                                       .Name(metric_name(name, metric))
                                       .Help(metric_help(config_name, metric))
                                       .Register(*registry_);

                std::string key;
                key.reserve(name.size() + 1 + metric.size());
                key.append(name);
                key.push_back('.');
                key.append(metric);

                state_gauges_.emplace(std::move(key), &family.Add({}));
            }
            collectors_.push_back(std::move(state));
        };

        add_collector(std::make_unique<ProcessMetricsCollector>());
        add_collector(std::make_unique<JemallocMetricsCollector>());
        add_collector(std::make_unique<GlibcMetricsCollector>());
        const auto gpu_options = options_.collectors.find("gpu");
        if (gpu_options != options_.collectors.end()) {
            auto devices = gpu_options->second.device_indices;
            if (devices.empty()) {
                devices.push_back(gpu_options->second.device_index);
            }
            std::sort(devices.begin(), devices.end());
            devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
            for (const auto device: devices) {
                add_collector(std::make_unique<GpuMetricsCollector>(device), "gpu");
            }
        }
    }

    void MetricsService::register_application_metrics() {
        if (options_.collect_http_metrics) {
            http_requests_family_ = &prometheus::BuildCounter()
                                             .Name(options_.metric_namespace + "_http_requests_total")
                                             .Help("Completed PolyGraph HTTP requests.")
                                             .Register(*registry_);
            http_duration_family_ = &prometheus::BuildHistogram()
                                             .Name(options_.metric_namespace + "_http_request_duration_seconds")
                                             .Help("PolyGraph HTTP request duration.")
                                             .Register(*registry_);
            http_in_progress_family_ = &prometheus::BuildGauge()
                                                .Name(options_.metric_namespace + "_http_requests_in_progress")
                                                .Help("Currently executing PolyGraph HTTP requests.")
                                                .Register(*registry_);
            http_oldest_in_progress_family_ =
                    &prometheus::BuildGauge()
                             .Name(options_.metric_namespace + "_http_request_oldest_in_progress_seconds")
                             .Help("Age in seconds of the oldest currently executing request for a route/method.")
                             .Register(*registry_);
        }

        if (options_.collect_event_metrics) {
            event_family_ = &prometheus::BuildCounter()
                                     .Name(options_.metric_namespace + "_events_total")
                                     .Help("Structured events emitted by severity.")
                                     .Register(*registry_);
            for (int level = static_cast<int>(EventLevel::Trace); level <= static_cast<int>(EventLevel::Error);
                 ++level) {
                const auto event_level = static_cast<EventLevel>(level);
                event_series_.emplace(level, &event_family_->Add({{"level", observability::to_string(event_level)}}));
            }
        }

        // Fixed, label-free gauges keep cardinality bounded and make the values cheap
        // to expose on every scrape. Values are updated only when a projection is built.
        auto add_gauge = [&](const std::string &suffix, const std::string &help) -> prometheus::Gauge * {
            auto &family = prometheus::BuildGauge()
                                   .Name(options_.metric_namespace + "_gpu_projection_" + suffix)
                                   .Help(help)
                                   .Register(*registry_);
            return &family.Add({});
        };
        gpu_projection_vertices_ = add_gauge("vertices", "Vertices represented by the active GPU projection.");
        gpu_projection_input_edges_ =
                add_gauge("input_edges", "Logical edge endpoint pairs supplied to the GPU projection builder.");
        gpu_projection_raw_adjacency_pairs_ =
                add_gauge("raw_adjacency_pairs", "Adjacency pairs after optional mirroring and before deduplication.");
        gpu_projection_unique_adjacency_pairs_ =
                add_gauge("unique_adjacency_pairs", "Unique adjacency pairs supplied to cuGraph.");
        gpu_projection_duplicate_pairs_removed_ =
                add_gauge("duplicate_adjacency_pairs_removed",
                          "Duplicate adjacency pairs removed while constructing the GPU projection.");
        gpu_projection_host_id_mapping_required_bytes_ =
                add_gauge("host_id_mapping_required_bytes",
                          "Payload bytes required by the retained dense-index to GlobalId vector.");
        gpu_projection_host_id_mapping_allocated_bytes_ =
                add_gauge("host_id_mapping_allocated_bytes",
                          "Allocated capacity in bytes of the retained dense-index to GlobalId vector.");
        gpu_projection_device_vertex_input_bytes_ =
                add_gauge("device_vertex_input_bytes",
                          "Bytes in the dense vertex input buffer used to construct the cuGraph graph.");
        gpu_projection_device_edge_input_bytes_ =
                add_gauge("device_edge_input_bytes",
                          "Bytes in source and destination input buffers used to construct the cuGraph graph.");
        gpu_projection_build_duration_seconds_ =
                add_gauge("build_duration_seconds", "Wall-clock duration of the latest GPU projection build.");
        gpu_projection_mirrored_ =
                add_gauge("mirrored_for_undirected_expansion",
                          "Whether reverse adjacency pairs were generated for undirected expansion.");

        auto add_mmap_gauge = [&](const std::string &suffix, const std::string &help) -> prometheus::Gauge * {
            auto &family = prometheus::BuildGauge()
                                   .Name(options_.metric_namespace + "_mmap_cache_" + suffix)
                                   .Help(help)
                                   .Register(*registry_);
            return &family.Add({});
        };
        mmap_cache_mapped_bytes_ = add_mmap_gauge("mapped_bytes", "Bytes mapped by the PolyGraph mmap cache.");
        mmap_cache_index_bytes_ = add_mmap_gauge("index_bytes", "Bytes occupied by the mmap cache source index.");
        mmap_cache_neighbor_bytes_ = add_mmap_gauge("neighbor_bytes", "Bytes occupied by mmap cached adjacency.");
        mmap_cache_vertices_ = add_mmap_gauge("vertices", "Vertices with adjacency present in the mmap cache.");
        mmap_cache_relations_ = add_mmap_gauge("relations", "Adjacency entries present in the mmap cache.");
    }

    void MetricsService::collect_state_metrics() {
        const auto now = std::chrono::steady_clock::now();

        for (auto &state: collectors_) {
            if (!state.is_available) {
                continue;
            }

            const auto ttl = std::chrono::milliseconds(state.options.cache_ttl_ms);
            const bool cache_valid = state.has_collected && now - state.last_collection < ttl;
            if (!cache_valid) {
                const auto started = std::chrono::steady_clock::now();
                try {
                    state.cached_values = state.collector->collect(state.options.metrics);
                } catch (...) {
                    state.errors->Increment();
                }
                // Successful empty snapshots and failures both respect the configured
                // TTL, preventing a broken collector from being retried on every scrape.
                state.last_collection = now;
                state.has_collected = true;
                const double seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                state.duration->Observe(seconds);
            }

            const std::string collector_name(state.collector->name());
            for (const auto &[metric, value]: state.cached_values) {
                std::string key;
                key.reserve(collector_name.size() + 1 + metric.size());
                key.append(collector_name);
                key.push_back('.');
                key.append(metric);

                const auto gauge = state_gauges_.find(key);
                if (gauge != state_gauges_.end()) {
                    gauge->second->Set(value);
                }
            }
        }
    }

    void MetricsService::update_active_http_metrics() {
        if (!options_.collect_http_metrics || http_in_progress_family_ == nullptr) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(application_mutex_);
        for (auto &[key, series]: active_http_series_) {
            (void) key;
            series.in_progress->Set(static_cast<double>(series.requests.size()));
            double oldest = 0.0;
            for (const auto &[request_id, started]: series.requests) {
                (void) request_id;
                oldest = std::max(oldest, std::chrono::duration<double>(now - started).count());
            }
            series.oldest_seconds->Set(oldest);
        }
    }

    std::string MetricsService::render() {
        if (!options_.enabled) {
            return {};
        }

        std::scoped_lock lock(scrape_mutex_);
        collect_state_metrics();
        update_active_http_metrics();
        prometheus::TextSerializer serializer;
        return serializer.Serialize(registry_->Collect());
    }

    MetricsService::HttpSeries &MetricsService::http_series(const std::string_view route, const std::string_view method,
                                                            const std::string_view status) {
        const std::string key = http_key(route, method, status);
        if (auto existing = http_series_.find(key); existing != http_series_.end()) {
            return existing->second;
        }

        const prometheus::Labels request_labels{
                {"route", std::string(route)}, {"method", std::string(method)}, {"status_class", std::string(status)}};
        const prometheus::Labels duration_labels{
                {"route", std::string(route)}, {"method", std::string(method)}, {"status_class", std::string(status)}};

        HttpSeries series;
        series.requests = &http_requests_family_->Add(request_labels);
        series.duration = &http_duration_family_->Add(duration_labels, options_.http_duration.seconds_buckets);
        return http_series_.emplace(key, series).first->second;
    }


    std::uint64_t MetricsService::begin_http(const std::string_view route, const std::string_view method) {
        if (!options_.enabled || !options_.collect_http_metrics || http_in_progress_family_ == nullptr) {
            return 0;
        }

        std::scoped_lock lock(application_mutex_);
        std::string key;
        key.reserve(route.size() + method.size() + 1);
        key.append(route);
        key.push_back('\n');
        key.append(method);

        auto [iterator, inserted] = active_http_series_.try_emplace(key);
        auto &series = iterator->second;
        if (inserted) {
            const prometheus::Labels labels{{"route", std::string(route)}, {"method", std::string(method)}};
            series.in_progress = &http_in_progress_family_->Add(labels);
            series.oldest_seconds = &http_oldest_in_progress_family_->Add(labels);
        }

        const std::uint64_t request_id = next_http_request_id_++;
        series.requests.emplace(request_id, std::chrono::steady_clock::now());
        series.in_progress->Set(static_cast<double>(series.requests.size()));
        return request_id;
    }

    void MetricsService::finish_http(const std::uint64_t request_id, const std::string_view route,
                                     const std::string_view method, const int status_code,
                                     const double duration_seconds) {
        if (!options_.enabled || !options_.collect_http_metrics || http_requests_family_ == nullptr) {
            return;
        }

        std::scoped_lock lock(application_mutex_);
        std::string active_key;
        active_key.reserve(route.size() + method.size() + 1);
        active_key.append(route);
        active_key.push_back('\n');
        active_key.append(method);
        if (const auto active = active_http_series_.find(active_key); active != active_http_series_.end()) {
            if (request_id != 0) {
                active->second.requests.erase(request_id);
            }
            active->second.in_progress->Set(static_cast<double>(active->second.requests.size()));
            if (active->second.requests.empty()) {
                active->second.oldest_seconds->Set(0.0);
            }
        }

        auto &series = http_series(route, method, status_class(status_code));
        series.requests->Increment();
        series.duration->Observe(duration_seconds);
    }

    void MetricsService::observe_http(const std::string_view route, const std::string_view method,
                                      const int status_code, const double duration_seconds) {
        if (!options_.enabled || !options_.collect_http_metrics || http_requests_family_ == nullptr) {
            return;
        }

        std::scoped_lock lock(application_mutex_);
        auto &series = http_series(route, method, status_class(status_code));
        series.requests->Increment();
        series.duration->Observe(duration_seconds);
    }

    void MetricsService::observe_event(const EventLevel level) {
        if (!options_.enabled || !options_.collect_event_metrics || event_family_ == nullptr) {
            return;
        }

        const auto iterator = event_series_.find(static_cast<int>(level));
        if (iterator != event_series_.end()) {
            iterator->second->Increment();
        }
    }

    void MetricsService::set_gpu_projection(const observability::GpuProjectionMetrics &snapshot) {
        if (!options_.enabled || gpu_projection_vertices_ == nullptr) {
            return;
        }
        std::scoped_lock lock(application_mutex_);
        gpu_projection_vertices_->Set(static_cast<double>(snapshot.vertices));
        gpu_projection_input_edges_->Set(static_cast<double>(snapshot.input_edges));
        gpu_projection_raw_adjacency_pairs_->Set(static_cast<double>(snapshot.raw_adjacency_pairs));
        gpu_projection_unique_adjacency_pairs_->Set(static_cast<double>(snapshot.unique_adjacency_pairs));
        gpu_projection_duplicate_pairs_removed_->Set(static_cast<double>(snapshot.duplicate_adjacency_pairs_removed));
        gpu_projection_host_id_mapping_required_bytes_->Set(
                static_cast<double>(snapshot.host_id_mapping_required_bytes));
        gpu_projection_host_id_mapping_allocated_bytes_->Set(
                static_cast<double>(snapshot.host_id_mapping_allocated_bytes));
        gpu_projection_device_vertex_input_bytes_->Set(static_cast<double>(snapshot.device_vertex_input_bytes));
        gpu_projection_device_edge_input_bytes_->Set(static_cast<double>(snapshot.device_edge_input_bytes));
        gpu_projection_build_duration_seconds_->Set(snapshot.build_duration_seconds);
        gpu_projection_mirrored_->Set(snapshot.mirrored_for_undirected_expansion ? 1.0 : 0.0);
    }

    void MetricsService::set_mmap_cache(const observability::MMapCacheMetrics &snapshot) {
        if (!options_.enabled || mmap_cache_mapped_bytes_ == nullptr) {
            return;
        }
        std::scoped_lock lock(application_mutex_);
        mmap_cache_mapped_bytes_->Set(static_cast<double>(snapshot.mapped_bytes));
        mmap_cache_index_bytes_->Set(static_cast<double>(snapshot.index_bytes));
        mmap_cache_neighbor_bytes_->Set(static_cast<double>(snapshot.neighbor_bytes));
        mmap_cache_vertices_->Set(static_cast<double>(snapshot.vertices));
        mmap_cache_relations_->Set(static_cast<double>(snapshot.relations));
    }

} // namespace polygraph::observability::metrics
