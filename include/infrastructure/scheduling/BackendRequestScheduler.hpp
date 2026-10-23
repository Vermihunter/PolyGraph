/**
 * @file include/infrastructure/scheduling/BackendRequestScheduler.hpp
 * @brief Server-global admission control and fair scheduling for remote backend requests.
 */
#ifndef POLYGRAPH_INFRASTRUCTURE_SCHEDULING_BACKEND_REQUEST_SCHEDULER_HPP
#define POLYGRAPH_INFRASTRUCTURE_SCHEDULING_BACKEND_REQUEST_SCHEDULER_HPP

#include "events/Events.hpp"
#include "events/EventContext.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace polygraph {

enum class BackendSchedulingPolicy {
    Fifo,
    RoundRobin,
    Priority,
    WeightedFair,
    PriorityWeightedFair,
};

[[nodiscard]] std::string_view to_string(BackendSchedulingPolicy policy) noexcept;
[[nodiscard]] BackendSchedulingPolicy parse_backend_scheduling_policy(std::string_view value);
[[nodiscard]] std::uint64_t next_backend_scheduler_query_id() noexcept;

struct BackendSchedulerConfig {
    /// Disable the scheduler to recover the original direct backend hot path.
    bool enabled = false;
    std::size_t max_in_flight = 64;
    /// Zero means unbounded. Producers block before enqueueing once the queue reaches this size.
    std::size_t max_queued = 4096;
    /// Zero means unbounded. Prevents one BFS from filling the complete global queue.
    std::size_t max_queued_per_query = 512;
    BackendSchedulingPolicy policy = BackendSchedulingPolicy::PriorityWeightedFair;
    /// Priority gains one point after each complete aging interval. Zero disables aging.
    std::size_t priority_aging_ms = 5000;
};

/** @brief Per-BFS scheduling hints. Global backend limits always take precedence. */
struct BackendRequestScheduling {
    std::uint64_t query_id = 0;
    /// Maximum scheduler slots this BFS may occupy concurrently on this backend.
    std::size_t max_in_flight = std::numeric_limits<std::size_t>::max();
    /// Higher values are preferred by priority-aware policies. Negative values are allowed.
    int priority = 0;
    /// Relative share used by weighted-fair policies. Must be greater than zero.
    std::size_t weight = 1;
};

struct BackendSchedulerSnapshot {
    std::size_t in_flight = 0;
    std::size_t queued = 0;
    std::size_t active_queries = 0;
    std::size_t max_in_flight = 0;
};

class BackendRequestScheduler final {
  public:
    class Lease final {
      public:
        Lease() noexcept = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        [[nodiscard]] std::chrono::nanoseconds queue_wait() const noexcept { return queue_wait_; }

      private:
        friend class BackendRequestScheduler;
        Lease(BackendRequestScheduler* owner,
              std::uint64_t query_id,
              std::chrono::nanoseconds queue_wait) noexcept;
        void reset() noexcept;

        BackendRequestScheduler* owner_ = nullptr;
        std::uint64_t query_id_ = 0;
        std::chrono::nanoseconds queue_wait_{};
    };

    BackendRequestScheduler(std::string backend_name, BackendSchedulerConfig config);
    ~BackendRequestScheduler();

    BackendRequestScheduler(const BackendRequestScheduler&) = delete;
    BackendRequestScheduler& operator=(const BackendRequestScheduler&) = delete;

    [[nodiscard]] Lease acquire(const BackendRequestScheduling& scheduling);

    template<class Function>
    decltype(auto) run(const BackendRequestScheduling& scheduling,
                       std::string_view operation_name,
                       Function&& operation) {
        [[maybe_unused]] events::ScopedQueryExecutionContext event_context(scheduling.query_id);
        if (!config_.enabled) {
            if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
                std::invoke(std::forward<Function>(operation));
                return;
            } else {
                return std::invoke(std::forward<Function>(operation));
            }
        }

        Lease lease = acquire(scheduling);
        const double queue_wait_ms =
            std::chrono::duration<double, std::milli>(lease.queue_wait()).count();
        const BackendSchedulerSnapshot state = snapshot();
        (void)queue_wait_ms;
        (void)state;
        (void)operation_name;
        EVENT_SCOPE_INFO(operation_span,
                         "backend.scheduler.operation",
                         {"backend", backend_name_},
                         {"operation", std::string(operation_name)},
                         {"query_execution_id", scheduling.query_id},
                         {"priority", scheduling.priority},
                         {"weight", scheduling.weight},
                         {"query_max_in_flight", scheduling.max_in_flight},
                         {"queue_wait_ms", queue_wait_ms},
                         {"global_in_flight", state.in_flight},
                         {"global_limit", state.max_in_flight});

        try {
            if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
                std::invoke(std::forward<Function>(operation));
                EVENT_SCOPE_INFO_FIELD(operation_span, "status", "ok");
            } else {
                decltype(auto) result = std::invoke(std::forward<Function>(operation));
                EVENT_SCOPE_INFO_FIELD(operation_span, "status", "ok");
                return result;
            }
        } catch (const std::exception& error) {
            EVENT_SCOPE_INFO_FIELD(operation_span, "status", "failed");
            EVENT_SCOPE_INFO_FIELD(operation_span, "error", error.what());
            throw;
        } catch (...) {
            EVENT_SCOPE_INFO_FIELD(operation_span, "status", "failed");
            throw;
        }
    }

    [[nodiscard]] BackendSchedulerSnapshot snapshot() const;
    [[nodiscard]] const BackendSchedulerConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::string& backend_name() const noexcept { return backend_name_; }

  private:
    struct Impl;
    std::string backend_name_;
    BackendSchedulerConfig config_;
    std::unique_ptr<Impl> impl_;

    void release(std::uint64_t query_id) noexcept;
};

} // namespace polygraph

#endif // POLYGRAPH_INFRASTRUCTURE_SCHEDULING_BACKEND_REQUEST_SCHEDULER_HPP
