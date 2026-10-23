/**
 * @file include/utilities/ThreadPool.hpp
 * @brief Small fixed-size C++ thread pool for reusable background work.
 */

#ifndef POLYGRAPH_UTILITIES_THREAD_POOL_HPP
#define POLYGRAPH_UTILITIES_THREAD_POOL_HPP

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace polygraph {

/**
 * @brief Fixed-size worker pool accepting arbitrary callable jobs.
 *
 * BFS already exposes the desired worker count through BfsOptions. The current
 * backend batching implementation deliberately continues to use std::async;
 * this pool is ready for the later bounded-execution change.
 */
class ThreadPool {
  public:
    explicit ThreadPool(std::size_t thread_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool(ThreadPool &&) = delete;
    ThreadPool &operator=(ThreadPool &&) = delete;

    /** @brief Number of worker threads owned by the pool. */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Submit a job and receive a future for its result.
     * @throws std::runtime_error when submission happens after shutdown starts.
     */
    template<typename Function, typename... Arguments>
    auto submit(Function &&function, Arguments &&...arguments)
            -> std::future<std::invoke_result_t<Function, Arguments...>> {
        using result_type = std::invoke_result_t<Function, Arguments...>;

        auto task = std::make_shared<std::packaged_task<result_type()>>(
                std::bind(std::forward<Function>(function), std::forward<Arguments>(arguments)...));
        std::future<result_type> result = task->get_future();

        enqueue([task] { (*task)(); });
        return result;
    }

    /** @brief Block until the queue is empty and every active job has finished. */
    void wait_idle();

  private:
    void enqueue(std::function<void()> job);
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable idle_;
    std::size_t active_workers_{};
    bool stopping_{};
};

} // namespace polygraph

#endif // POLYGRAPH_UTILITIES_THREAD_POOL_HPP
