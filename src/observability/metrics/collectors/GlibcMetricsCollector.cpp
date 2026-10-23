#include "observability/metrics/collectors/GlibcMetricsCollector.hpp"

#include <malloc.h>
#include <string_view>
#include <unordered_set>

namespace polygraph::observability::metrics {

    bool GlibcMetricsCollector::available() const noexcept {
#if defined(__GLIBC__) && !defined(POLYGRAPH_USE_JEMALLOC)
        return true;
#else
        return false;
#endif
    }

    MetricValues GlibcMetricsCollector::collect(const std::span<const std::string> requested) {
        MetricValues result;
#if defined(__GLIBC__) && !defined(POLYGRAPH_USE_JEMALLOC)
        const std::unordered_set<std::string_view> selected(requested.begin(), requested.end());
        const struct mallinfo2 info = ::mallinfo2();

        const auto add = [&](const std::string_view name, const auto value) {
            if (selected.contains(name)) {
                result.emplace(std::string(name), static_cast<double>(value));
            }
        };

        add("arena_bytes", info.arena);
        add("allocated_bytes", info.uordblks);
        add("free_bytes", info.fordblks);
        add("mmap_bytes", info.hblkhd);
        add("mmap_regions", info.hblks);
        add("releasable_bytes", info.keepcost);
#else
        (void) requested;
#endif
        return result;
    }

} // namespace polygraph::observability::metrics
