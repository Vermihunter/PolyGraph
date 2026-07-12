/**
 * @file include/utilities/Profiling.hpp
 * @brief Declarations for profiling in the utility module.
 */

#ifndef CPGDB_UTILITIES_PROFILING_HPP
#define CPGDB_UTILITIES_PROFILING_HPP

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
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

    static void add(const char* name, std::uint64_t inclusive_ns, std::uint64_t self_ns);
    static void dump(std::ostream& output = std::cerr);
};

struct StackFrame {
    const char* name;
    std::chrono::steady_clock::time_point start;
    std::uint64_t child_ns = 0;
};

extern thread_local std::vector<StackFrame> stack;

class Scope {
  public:
    explicit Scope(const char* name);
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope();

  private:
    bool active_ = true;
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
#define CPGDB_PROFILE_FUNCTION()                                                                   \
    ::cpgdb::prof::Scope cpgdb_prof_scope_##__LINE__(CPGDB_FUNCTION_NAME)
#else
#define CPGDB_PROFILE_SCOPE(name)                                                                  \
    do {                                                                                           \
    } while (false)
#define CPGDB_PROFILE_FUNCTION()                                                                   \
    do {                                                                                           \
    } while (false)
#endif

#endif // CPGDB_UTILITIES_PROFILING_HPP
