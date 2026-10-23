/**
 * @file include/events/EventContext.hpp
 * @brief Per-thread correlation context for structured execution events.
 */
#pragma once

#include <cstdint>

namespace polygraph::events {

namespace detail {
inline thread_local std::uint64_t query_execution_id = 0;
}

/**
 * @brief Installs a query-execution ID for events emitted by the current thread.
 *
 * Contexts are nestable. Destruction restores the previous value, which makes
 * the helper safe around scheduler operations and backend worker tasks.
 */
class ScopedQueryExecutionContext final {
  public:
    explicit ScopedQueryExecutionContext(const std::uint64_t query_execution_id) noexcept
        : previous_(detail::query_execution_id) {
        detail::query_execution_id = query_execution_id;
    }

    ScopedQueryExecutionContext(const ScopedQueryExecutionContext&) = delete;
    ScopedQueryExecutionContext& operator=(const ScopedQueryExecutionContext&) = delete;
    ScopedQueryExecutionContext(ScopedQueryExecutionContext&&) = delete;
    ScopedQueryExecutionContext& operator=(ScopedQueryExecutionContext&&) = delete;

    ~ScopedQueryExecutionContext() noexcept {
        detail::query_execution_id = previous_;
    }

  private:
    std::uint64_t previous_{};
};

[[nodiscard]] inline std::uint64_t current_query_execution_id() noexcept {
    return detail::query_execution_id;
}

} // namespace polygraph::events
