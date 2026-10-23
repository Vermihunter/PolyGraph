#include "observability/metrics/collectors/JemallocMetricsCollector.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#ifdef POLYGRAPH_USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

namespace polygraph::observability::metrics {
    namespace {

#ifdef POLYGRAPH_USE_JEMALLOC
        [[nodiscard]] bool read_size(const char *key, std::size_t &value) {
            std::size_t size = sizeof(value);
            return mallctl(key, &value, &size, nullptr, 0) == 0;
        }

        [[nodiscard]] bool refresh_epoch() {
            std::uint64_t epoch = 1;
            std::size_t size = sizeof(epoch);
            return mallctl("epoch", &epoch, &size, &epoch, sizeof(epoch)) == 0;
        }
#endif

    } // namespace

    bool JemallocMetricsCollector::available() const noexcept {
#ifdef POLYGRAPH_USE_JEMALLOC
        const char *version = nullptr;
        std::size_t size = sizeof(version);
        return mallctl("version", &version, &size, nullptr, 0) == 0;
#else
        return false;
#endif
    }

    MetricValues JemallocMetricsCollector::collect(const std::span<const std::string> requested) {
        MetricValues result;
#ifdef POLYGRAPH_USE_JEMALLOC
        if (!refresh_epoch()) {
            return result;
        }

        const std::unordered_set<std::string_view> selected(requested.begin(), requested.end());
        const auto add_size = [&](const std::string_view metric, const char *key) {
            if (!selected.contains(metric)) {
                return;
            }
            std::size_t value = 0;
            if (read_size(key, value)) {
                result.emplace(std::string(metric), static_cast<double>(value));
            }
        };

        add_size("allocated_bytes", "stats.allocated");
        add_size("active_bytes", "stats.active");
        add_size("resident_bytes", "stats.resident");
        add_size("mapped_bytes", "stats.mapped");
        add_size("retained_bytes", "stats.retained");
        add_size("metadata_bytes", "stats.metadata");

        std::size_t page_size = 0;
        if ((selected.contains("dirty_bytes") || selected.contains("muzzy_bytes")) &&
            read_size("arenas.page", page_size)) {
            const auto arena_prefix = std::string("stats.arenas.") + std::to_string(MALLCTL_ARENAS_ALL) + ".";
            std::size_t pages = 0;
            if (selected.contains("dirty_bytes") && read_size((arena_prefix + "pdirty").c_str(), pages)) {
                result.emplace("dirty_bytes", static_cast<double>(pages * page_size));
            }
            pages = 0;
            if (selected.contains("muzzy_bytes") && read_size((arena_prefix + "pmuzzy").c_str(), pages)) {
                result.emplace("muzzy_bytes", static_cast<double>(pages * page_size));
            }
        }
#else
        (void) requested;
#endif
        return result;
    }

} // namespace polygraph::observability::metrics
