/**
 * @file include/observability/detail/PeriodicFlusher.hpp
 * @brief Small stoppable worker used by independent observability outputs.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace polygraph::observability::detail {

class PeriodicFlusher final {
  public:
    PeriodicFlusher() = default;
    PeriodicFlusher(const PeriodicFlusher&) = delete;
    PeriodicFlusher& operator=(const PeriodicFlusher&) = delete;

    ~PeriodicFlusher() { stop(); }

    template <typename Flush>
    void start(const std::uint64_t period_ms, Flush&& flush) {
        stop();
        if (period_ms == 0) {
            return;
        }

        const auto period = std::chrono::milliseconds{period_ms};
        worker_ = std::jthread(
            [this, period, flush = std::forward<Flush>(flush)](const std::stop_token stop_token) mutable {
                std::unique_lock lock(mutex_);
                while (!stop_token.stop_requested()) {
                    wake_.wait_for(lock, stop_token, period, [] { return false; });
                    if (stop_token.stop_requested()) {
                        break;
                    }

                    lock.unlock();
                    flush();
                    lock.lock();
                }
            });
    }

    void stop() noexcept {
        if (!worker_.joinable()) {
            return;
        }
        worker_.request_stop();
        wake_.notify_all();
        worker_.join();
    }

  private:
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

} // namespace polygraph::observability::detail
