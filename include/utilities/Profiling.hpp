//
// Created by akos on 15. 6. 2026.
//

#ifndef CPGDB_PROFILING_HPP
#define CPGDB_PROFILING_HPP


#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpgdb::prof {

class Profiler {
public:
    struct Stats {
        std::uint64_t calls = 0;
        std::uint64_t inclusive_ns = 0;
        std::uint64_t self_ns = 0;
        std::uint64_t max_inclusive_ns = 0;
        std::uint64_t min_inclusive_ns = std::numeric_limits<std::uint64_t>::max();
    };

    static void add(const char* name,
                    std::uint64_t inclusive_ns,
                    std::uint64_t self_ns) {
        std::lock_guard<std::mutex> lock(mutex());

        auto& s = stats()[name];

        ++s.calls;
        s.inclusive_ns += inclusive_ns;
        s.self_ns += self_ns;
        s.max_inclusive_ns = std::max(s.max_inclusive_ns, inclusive_ns);
        s.min_inclusive_ns = std::min(s.min_inclusive_ns, inclusive_ns);
    }

    static void reset() {
        std::lock_guard<std::mutex> lock(mutex());
        stats().clear();
    }

    static void dump(std::ostream& os = std::cerr) {
        std::lock_guard<std::mutex> lock(mutex());

        struct Row {
            std::string name;
            Stats stats;
        };

        std::vector<Row> rows;
        rows.reserve(stats().size());

        for (const auto& [name, s] : stats()) {
            rows.push_back(Row{name, s});
        }

        std::sort(
            rows.begin(),
            rows.end(),
            [](const Row& a, const Row& b) {
                return a.stats.inclusive_ns > b.stats.inclusive_ns;
            }
        );

        std::uint64_t total_inclusive = 0;

        for (const auto& row : rows) {
            total_inclusive = std::max(total_inclusive, row.stats.inclusive_ns);
        }

        os << "\n================ CPGDB PROFILER ================\n";
        os << std::left
           << std::setw(8)  << "Calls"
           << std::setw(14) << "Incl ms"
           << std::setw(14) << "Self ms"
           << std::setw(12) << "Avg us"
           << std::setw(12) << "Max ms"
           << std::setw(9)  << "Incl %"
           << "Function\n";

        os << "------------------------------------------------\n";

        for (const auto& row : rows) {
            const auto& s = row.stats;

            const double incl_ms = static_cast<double>(s.inclusive_ns) / 1'000'000.0;
            const double self_ms = static_cast<double>(s.self_ns) / 1'000'000.0;
            const double avg_us =
                s.calls == 0
                    ? 0.0
                    : static_cast<double>(s.inclusive_ns) / static_cast<double>(s.calls) / 1'000.0;

            const double max_ms = static_cast<double>(s.max_inclusive_ns) / 1'000'000.0;

            const double pct =
                total_inclusive == 0
                    ? 0.0
                    : 100.0 * static_cast<double>(s.inclusive_ns) /
                        static_cast<double>(total_inclusive);

            os << std::left
               << std::setw(8)  << s.calls
               << std::setw(14) << std::fixed << std::setprecision(3) << incl_ms
               << std::setw(14) << std::fixed << std::setprecision(3) << self_ms
               << std::setw(12) << std::fixed << std::setprecision(3) << avg_us
               << std::setw(12) << std::fixed << std::setprecision(3) << max_ms
               << std::setw(9)  << std::fixed << std::setprecision(2) << pct
               << row.name
               << '\n';
        }

        os << "================================================\n\n";
    }

private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }

    static std::unordered_map<std::string, Stats>& stats() {
        static std::unordered_map<std::string, Stats> s;
        return s;
    }
};

struct StackFrame {
    const char* name;
    std::chrono::steady_clock::time_point start;
    std::uint64_t child_ns = 0;
};

inline thread_local std::vector<StackFrame> stack;

class Scope {
public:
    explicit Scope(const char* name)
        : active_(true) {
        stack.push_back(StackFrame{
            .name = name,
            .start = std::chrono::steady_clock::now(),
            .child_ns = 0
        });
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    ~Scope() {
        if (!active_) {
            return;
        }

        const auto end = std::chrono::steady_clock::now();

        auto frame = stack.back();
        stack.pop_back();

        const auto inclusive_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - frame.start
                ).count()
            );

        const auto self_ns =
            inclusive_ns >= frame.child_ns
                ? inclusive_ns - frame.child_ns
                : 0;

        if (!stack.empty()) {
            stack.back().child_ns += inclusive_ns;
        }

        Profiler::add(frame.name, inclusive_ns, self_ns);
    }

private:
    bool active_;
};

} // namespace cpgdb::prof

#if defined(__GNUC__) || defined(__clang__)
    #define CPGDB_FUNCTION_NAME __PRETTY_FUNCTION__
#elif defined(_MSC_VER)
    #define CPGDB_FUNCTION_NAME __FUNCSIG__
#else
    #define CPGDB_FUNCTION_NAME __func__
#endif

#ifdef CPGDB_ENABLE_PROFILING
    #define CPGDB_PROFILE_SCOPE(name) ::cpgdb::prof::Scope cpgdb_prof_scope_##__LINE__(name)
    #define CPGDB_PROFILE_FUNCTION() ::cpgdb::prof::Scope cpgdb_prof_scope_##__LINE__(CPGDB_FUNCTION_NAME)
#else
    #define CPGDB_PROFILE_SCOPE(name) do {} while (false)
    #define CPGDB_PROFILE_FUNCTION() do {} while (false)
#endif

#endif //CPGDB_PROFILING_HPP