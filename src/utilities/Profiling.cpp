/**
 * @file src/utilities/Profiling.cpp
 * @brief Implementation for profiling in the utility module.
 */

#include "utilities/Profiling.hpp"

#include <algorithm>
#include <iomanip>
#include <mutex>
#include <unordered_map>

namespace cpgdb::prof {
    namespace {

        std::mutex profiler_mutex;
        std::unordered_map<std::string, Profiler::Stats> profiler_stats;

    } // namespace

    thread_local std::vector<StackFrame> stack;

    void Profiler::add(const char *name, std::uint64_t inclusive_ns, std::uint64_t self_ns) {
        std::lock_guard lock(profiler_mutex);
        Stats &stats = profiler_stats[name];
        ++stats.calls;
        stats.inclusive_ns += inclusive_ns;
        stats.self_ns += self_ns;
        stats.max_inclusive_ns = std::max(stats.max_inclusive_ns, inclusive_ns);
        stats.min_inclusive_ns = std::min(stats.min_inclusive_ns, inclusive_ns);
    }

    void Profiler::dump(std::ostream &output) {
        struct Row {
            std::string name;
            Stats stats;
        };

        std::lock_guard lock(profiler_mutex);
        std::vector<Row> rows;
        rows.reserve(profiler_stats.size());
        for (const auto &[name, stats]: profiler_stats) {
            rows.push_back({name, stats});
        }
        std::ranges::sort(rows, std::greater{}, [](const Row &row) { return row.stats.inclusive_ns; });

        std::uint64_t total_inclusive = 0;
        for (const Row &row: rows) {
            total_inclusive = std::max(total_inclusive, row.stats.inclusive_ns);
        }

        output << "\n================ CPGDB PROFILER ================\n"
               << std::left << std::setw(8) << "Calls" << std::setw(14) << "Incl ms" << std::setw(14) << "Self ms"
               << std::setw(12) << "Avg us" << std::setw(12) << "Max ms" << std::setw(9) << "Incl %"
               << "Function\n"
               << "------------------------------------------------\n";

        for (const Row &row: rows) {
            const Stats &stats = row.stats;
            const double inclusive_ms = static_cast<double>(stats.inclusive_ns) / 1'000'000.0;
            const double self_ms = static_cast<double>(stats.self_ns) / 1'000'000.0;
            const double average_us =
                    stats.calls == 0 ? 0.0 : static_cast<double>(stats.inclusive_ns) / (double) stats.calls / 1'000.0;
            const double maximum_ms = static_cast<double>(stats.max_inclusive_ns) / 1'000'000.0;
            const double percentage =
                    total_inclusive == 0 ? 0.0
                                         : 100.0 * static_cast<double>(stats.inclusive_ns) / (double) total_inclusive;

            output << std::left << std::setw(8) << stats.calls << std::setw(14) << std::fixed << std::setprecision(3)
                   << inclusive_ms << std::setw(14) << self_ms << std::setw(12) << average_us << std::setw(12)
                   << maximum_ms << std::setw(9) << std::setprecision(2) << percentage << row.name << '\n';
        }

        output << "================================================\n\n";
    }

    Scope::Scope(const char *name) {
        stack.push_back(StackFrame{.name = name, .start = std::chrono::steady_clock::now(), .child_ns = 0});
    }

    Scope::~Scope() {
        if (!active_ || stack.empty()) {
            return;
        }

        const auto end = std::chrono::steady_clock::now();
        const StackFrame frame = stack.back();
        stack.pop_back();

        const auto inclusive_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - frame.start).count());
        const auto self_ns = inclusive_ns >= frame.child_ns ? inclusive_ns - frame.child_ns : 0;

        if (!stack.empty()) {
            stack.back().child_ns += inclusive_ns;
        }
        Profiler::add(frame.name, inclusive_ns, self_ns);
    }

} // namespace cpgdb::prof
