#include "observability/metrics/collectors/GpuMetricsCollector.hpp"

#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef POLYGRAPH_ENABLE_GPU
#include <nvml.h>
#include <unistd.h>
#endif

namespace polygraph::observability::metrics {

    struct GpuMetricsCollector::State {
        unsigned int device_index = 0;
        bool initialized = false;
        bool device_available = false;
#ifdef POLYGRAPH_ENABLE_GPU
        nvmlDevice_t device{};
#endif
    };

    GpuMetricsCollector::GpuMetricsCollector(const unsigned int device_index) :
        state_(std::make_unique<State>()),
        collector_name_(device_index == 0 ? "gpu" : "gpu" + std::to_string(device_index)) {
        state_->device_index = device_index;
#ifdef POLYGRAPH_ENABLE_GPU
        if (nvmlInit_v2() != NVML_SUCCESS) {
            return;
        }
        state_->initialized = true;
        state_->device_available = nvmlDeviceGetHandleByIndex_v2(device_index, &state_->device) == NVML_SUCCESS;
#endif
    }

    GpuMetricsCollector::~GpuMetricsCollector() {
#ifdef POLYGRAPH_ENABLE_GPU
        if (state_ && state_->initialized) {
            nvmlShutdown();
        }
#endif
    }

    GpuMetricsCollector::GpuMetricsCollector(GpuMetricsCollector &&) noexcept = default;
    GpuMetricsCollector &GpuMetricsCollector::operator=(GpuMetricsCollector &&) noexcept = default;

    bool GpuMetricsCollector::available() const noexcept { return state_ && state_->device_available; }

    MetricValues GpuMetricsCollector::collect(const std::span<const std::string> requested) {
        MetricValues result;
#ifdef POLYGRAPH_ENABLE_GPU
        if (!available()) {
            return result;
        }

        const std::unordered_set<std::string_view> selected(requested.begin(), requested.end());

        const bool needs_memory = selected.contains("memory_total_bytes") || selected.contains("memory_used_bytes") ||
                                  selected.contains("memory_free_bytes");
        if (needs_memory) {
            nvmlMemory_t memory{};
            if (nvmlDeviceGetMemoryInfo(state_->device, &memory) == NVML_SUCCESS) {
                if (selected.contains("memory_total_bytes")) {
                    result.emplace("memory_total_bytes", static_cast<double>(memory.total));
                }
                if (selected.contains("memory_used_bytes")) {
                    result.emplace("memory_used_bytes", static_cast<double>(memory.used));
                }
                if (selected.contains("memory_free_bytes")) {
                    result.emplace("memory_free_bytes", static_cast<double>(memory.free));
                }
            }
        }

        if (selected.contains("utilization_percent") || selected.contains("memory_utilization_percent")) {
            nvmlUtilization_t utilization{};
            if (nvmlDeviceGetUtilizationRates(state_->device, &utilization) == NVML_SUCCESS) {
                if (selected.contains("utilization_percent")) {
                    result.emplace("utilization_percent", static_cast<double>(utilization.gpu));
                }
                if (selected.contains("memory_utilization_percent")) {
                    result.emplace("memory_utilization_percent", static_cast<double>(utilization.memory));
                }
            }
        }

        if (selected.contains("power_usage_watts")) {
            unsigned int milliwatts = 0;
            if (nvmlDeviceGetPowerUsage(state_->device, &milliwatts) == NVML_SUCCESS) {
                result.emplace("power_usage_watts", static_cast<double>(milliwatts) / 1000.0);
            }
        }

        if (selected.contains("power_limit_watts")) {
            unsigned int milliwatts = 0;
            if (nvmlDeviceGetEnforcedPowerLimit(state_->device, &milliwatts) == NVML_SUCCESS) {
                result.emplace("power_limit_watts", static_cast<double>(milliwatts) / 1000.0);
            }
        }

        if (selected.contains("temperature_celsius")) {
            unsigned int temperature = 0;
            if (nvmlDeviceGetTemperature(state_->device, NVML_TEMPERATURE_GPU, &temperature) == NVML_SUCCESS) {
                result.emplace("temperature_celsius", static_cast<double>(temperature));
            }
        }

        if (selected.contains("fan_speed_percent")) {
            unsigned int percent = 0;
            if (nvmlDeviceGetFanSpeed(state_->device, &percent) == NVML_SUCCESS) {
                result.emplace("fan_speed_percent", static_cast<double>(percent));
            }
        }

        const auto add_clock = [&](const std::string_view metric, const nvmlClockType_t type) {
            if (!selected.contains(metric)) {
                return;
            }
            unsigned int mhz = 0;
            if (nvmlDeviceGetClockInfo(state_->device, type, &mhz) == NVML_SUCCESS) {
                result.emplace(std::string(metric), static_cast<double>(mhz) * 1'000'000.0);
            }
        };
        add_clock("graphics_clock_hz", NVML_CLOCK_GRAPHICS);
        add_clock("sm_clock_hz", NVML_CLOCK_SM);
        add_clock("memory_clock_hz", NVML_CLOCK_MEM);

        if (selected.contains("compute_processes") || selected.contains("process_memory_bytes")) {
            unsigned int count = 0;
            nvmlReturn_t status = nvmlDeviceGetComputeRunningProcesses_v3(state_->device, &count, nullptr);
            if (status == NVML_SUCCESS || status == NVML_ERROR_INSUFFICIENT_SIZE) {
                if (count == 0) {
                    if (selected.contains("compute_processes")) {
                        result.emplace("compute_processes", 0.0);
                    }
                    if (selected.contains("process_memory_bytes")) {
                        result.emplace("process_memory_bytes", 0.0);
                    }
                } else {
                    // NVML documents the process list as dynamic. Keep spare capacity so a
                    // process that appears between the sizing and data calls does not make
                    // the scrape fail unnecessarily.
                    count += 8;
                    std::vector<nvmlProcessInfo_t> processes(count);
                    status = nvmlDeviceGetComputeRunningProcesses_v3(state_->device, &count, processes.data());
                    if (status == NVML_SUCCESS) {
                        if (selected.contains("compute_processes")) {
                            result.emplace("compute_processes", static_cast<double>(count));
                        }
                        if (selected.contains("process_memory_bytes")) {
                            const auto current_pid = static_cast<unsigned int>(getpid());
                            double bytes = 0.0;
                            for (unsigned int index = 0; index < count; ++index) {
                                const auto &process = processes[index];
                                if (process.pid == current_pid && process.usedGpuMemory != NVML_VALUE_NOT_AVAILABLE) {
                                    bytes += static_cast<double>(process.usedGpuMemory);
                                }
                            }
                            result.emplace("process_memory_bytes", bytes);
                        }
                    }
                }
            }
        }
#else
        (void) requested;
#endif
        return result;
    }

} // namespace polygraph::observability::metrics
