/**
 * @file src/utilities/ThreadPool.cpp
 * @brief Implementation of the reusable fixed-size worker pool.
 */

#include "utilities/ThreadPool.hpp"

#include <stdexcept>
#include <utility>

namespace polygraph {

    ThreadPool::ThreadPool(std::size_t thread_count) {
        if (thread_count == 0) {
            throw std::invalid_argument("ThreadPool requires at least one worker thread");
        }

        workers_.reserve(thread_count);
        try {
            for (std::size_t index = 0; index < thread_count; ++index) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            work_available_.notify_all();
            for (std::thread &worker: workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw;
        }
    }

    ThreadPool::~ThreadPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_available_.notify_all();

        for (std::thread &worker: workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    std::size_t ThreadPool::size() const noexcept { return workers_.size(); }

    void ThreadPool::wait_idle() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return jobs_.empty() && active_workers_ == 0; });
    }

    void ThreadPool::enqueue(std::function<void()> job) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("Cannot submit work to a stopped ThreadPool");
            }
            jobs_.push(std::move(job));
        }
        work_available_.notify_one();
    }

    void ThreadPool::worker_loop() {
        while (true) {
            std::function<void()> job;

            {
                std::unique_lock lock(mutex_);
                work_available_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });

                if (stopping_ && jobs_.empty()) {
                    return;
                }

                job = std::move(jobs_.front());
                jobs_.pop();
                ++active_workers_;
            }

            try {
                job();
            } catch (...) {
                // packaged_task transports callable exceptions through its future.
                // The guard also prevents a custom queued job from killing a worker.
            }

            {
                std::lock_guard lock(mutex_);
                --active_workers_;
                if (jobs_.empty() && active_workers_ == 0) {
                    idle_.notify_all();
                }
            }
        }
    }

} // namespace polygraph
