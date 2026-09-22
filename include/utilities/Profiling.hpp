/**
 * @file include/utilities/Profiling.hpp
 * @brief Declarations for profiling in the utility module.
 */

#ifndef POLYGRAPH_UTILITIES_PROFILING_HPP
#define POLYGRAPH_UTILITIES_PROFILING_HPP

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace polygraph::prof {

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

} // namespace polygraph::prof

#if defined(__GNUC__) || defined(__clang__)
#define POLYGRAPH_FUNCTION_NAME __PRETTY_FUNCTION__
#elif defined(_MSC_VER)
#define POLYGRAPH_FUNCTION_NAME __FUNCSIG__
#else
#define POLYGRAPH_FUNCTION_NAME __func__
#endif

#ifdef POLYGRAPH_ENABLE_PROFILING
#define POLYGRAPH_PROFILE_SCOPE(name) ::polygraph::prof::Scope polygraph_prof_scope_##__LINE__(name)
#define POLYGRAPH_PROFILE_FUNCTION()                                                                   \
    ::polygraph::prof::Scope polygraph_prof_scope_##__LINE__(POLYGRAPH_FUNCTION_NAME)
#else
#define POLYGRAPH_PROFILE_SCOPE(name)                                                                  \
    do {                                                                                           \
    } while (false)
#define POLYGRAPH_PROFILE_FUNCTION()                                                                   \
    do {                                                                                           \
    } while (false)
#endif

#endif // POLYGRAPH_UTILITIES_PROFILING_HPP
