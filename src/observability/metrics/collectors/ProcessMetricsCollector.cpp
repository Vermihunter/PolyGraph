#include "observability/metrics/collectors/ProcessMetricsCollector.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unordered_map>
#include <unordered_set>

namespace polygraph::observability::metrics {
    namespace {

        struct CpuTimes {
            double user_seconds = 0.0;
            double system_seconds = 0.0;

            [[nodiscard]] double total_seconds() const noexcept { return user_seconds + system_seconds; }
        };

        [[nodiscard]] double timeval_to_seconds(const timeval &value) noexcept {
            return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
        }

        [[nodiscard]] bool read_cpu_times(CpuTimes &times) noexcept {
            rusage usage{};

            if (getrusage(RUSAGE_SELF, &usage) != 0) {
                return false;
            }

            times.user_seconds = timeval_to_seconds(usage.ru_utime);
            times.system_seconds = timeval_to_seconds(usage.ru_stime);

            return true;
        }

        [[nodiscard]] std::unordered_map<std::string, double> read_status() {
            std::ifstream input("/proc/self/status");
            std::unordered_map<std::string, double> values;
            std::string line;

            while (std::getline(input, line)) {
                const auto colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }

                const std::string key = line.substr(0, colon);
                std::istringstream value_stream(line.substr(colon + 1));

                double value = 0.0;
                std::string unit;

                if (!(value_stream >> value)) {
                    continue;
                }

                value_stream >> unit;

                if (unit == "kB") {
                    value *= 1024.0;
                }

                values.emplace(key, value);
            }

            return values;
        }

        [[nodiscard]] double open_file_descriptors() {
            std::error_code error;
            double count = 0;

            for (std::filesystem::directory_iterator iterator("/proc/self/fd", error), end; !error && iterator != end;
                 iterator.increment(error)) {
                ++count;
            }

            // The directory iterator itself owns one descriptor visible
            // in /proc/self/fd.
            return count > 0.0 ? count - 1.0 : 0.0;
        }

        [[nodiscard]] double max_file_descriptors() {
            rlimit limits{};

            return getrlimit(RLIMIT_NOFILE, &limits) == 0 ? static_cast<double>(limits.rlim_cur) : 0.0;
        }

    } // namespace

    bool ProcessMetricsCollector::available() const noexcept { return std::filesystem::exists("/proc/self/status"); }

    MetricValues ProcessMetricsCollector::collect(const std::span<const std::string> requested) {
        const std::unordered_set<std::string_view> selected(requested.begin(), requested.end());

        const bool needs_status =
                selected.contains("virtual_memory_bytes") || selected.contains("resident_memory_bytes") ||
                selected.contains("resident_anonymous_bytes") || selected.contains("resident_file_bytes") ||
                selected.contains("swap_bytes") || selected.contains("threads");

        const bool needs_cpu = selected.contains("cpu_user_seconds_total") ||
                               selected.contains("cpu_system_seconds_total") || selected.contains("cpu_seconds_total");

        const auto status = needs_status ? read_status() : std::unordered_map<std::string, double>{};

        CpuTimes cpu_times{};
        const bool has_cpu_times = needs_cpu && read_cpu_times(cpu_times);

        MetricValues result;

        const auto add_status = [&](const std::string_view metric, const std::string_view proc_key) {
            if (!selected.contains(metric)) {
                return;
            }

            const auto iterator = status.find(std::string(proc_key));

            if (iterator != status.end()) {
                result.emplace(std::string(metric), iterator->second);
            }
        };

        add_status("virtual_memory_bytes", "VmSize");
        add_status("resident_memory_bytes", "VmRSS");
        add_status("resident_anonymous_bytes", "RssAnon");
        add_status("resident_file_bytes", "RssFile");
        add_status("swap_bytes", "VmSwap");
        add_status("threads", "Threads");

        if (has_cpu_times) {
            if (selected.contains("cpu_user_seconds_total")) {
                result.emplace("cpu_user_seconds_total", cpu_times.user_seconds);
            }

            if (selected.contains("cpu_system_seconds_total")) {
                result.emplace("cpu_system_seconds_total", cpu_times.system_seconds);
            }

            if (selected.contains("cpu_seconds_total")) {
                result.emplace("cpu_seconds_total", cpu_times.total_seconds());
            }
        }

        if (selected.contains("open_file_descriptors")) {
            result.emplace("open_file_descriptors", open_file_descriptors());
        }

        if (selected.contains("max_file_descriptors")) {
            result.emplace("max_file_descriptors", max_file_descriptors());
        }

        return result;
    }

} // namespace polygraph::observability::metrics
