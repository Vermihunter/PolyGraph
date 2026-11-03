#include "infrastructure/scheduling/BackendRequestScheduler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace polygraph {
    namespace {

        constexpr auto wait_timeout = std::chrono::seconds(5);

        class OperationGate {
        public:
            void enter_and_wait() {
                std::unique_lock lock(mutex_);
                ++entered_;
                ++active_;
                maximum_active_ = std::max(maximum_active_, active_);
                changed_.notify_all();
                const bool released = changed_.wait_for(lock, wait_timeout, [this] { return released_; });
                --active_;
                if (!released) {
                    throw std::runtime_error("Timed out waiting for test operations to be released");
                }
            }

            [[nodiscard]] bool wait_for_entries(const std::size_t count) {
                std::unique_lock lock(mutex_);
                return changed_.wait_for(lock, wait_timeout, [this, count] { return entered_ >= count; });
            }

            void release() {
                {
                    std::scoped_lock lock(mutex_);
                    released_ = true;
                }
                changed_.notify_all();
            }

            [[nodiscard]] std::size_t maximum_active() const {
                std::scoped_lock lock(mutex_);
                return maximum_active_;
            }

        private:
            mutable std::mutex mutex_;
            std::condition_variable changed_;
            std::size_t entered_{};
            std::size_t active_{};
            std::size_t maximum_active_{};
            bool released_{};
        };

        class ReleaseGateOnExit {
        public:
            explicit ReleaseGateOnExit(OperationGate &gate) : gate_(gate) {}
            ~ReleaseGateOnExit() { gate_.release(); }

            ReleaseGateOnExit(const ReleaseGateOnExit &) = delete;
            ReleaseGateOnExit &operator=(const ReleaseGateOnExit &) = delete;

        private:
            OperationGate &gate_;
        };

        [[nodiscard]] bool wait_for_queue_size(const BackendRequestScheduler &scheduler, const std::size_t expected) {
            const auto deadline = std::chrono::steady_clock::now() + wait_timeout;
            while (scheduler.snapshot().queued != expected) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                std::this_thread::yield();
            }
            return true;
        }

        testing::AssertionResult finish_tasks(std::vector<std::future<void>> &tasks) {
            const auto deadline = std::chrono::steady_clock::now() + wait_timeout;
            for (auto &task: tasks) {
                if (task.wait_until(deadline) != std::future_status::ready) {
                    return testing::AssertionFailure() << "Scheduler operation did not finish before its deadline";
                }
                try {
                    task.get();
                } catch (const std::exception &error) {
                    return testing::AssertionFailure() << "Scheduler operation failed: " << error.what();
                } catch (...) {
                    return testing::AssertionFailure() << "Scheduler operation failed with an unknown exception";
                }
            }
            return testing::AssertionSuccess();
        }

        TEST(BackendRequestSchedulerTest, HonorsGlobalConcurrencyLimit) {
            BackendRequestScheduler scheduler("test",
                                              BackendSchedulerConfig{.enabled = true,
                                                                     .max_in_flight = 2,
                                                                     .max_queued = 32,
                                                                     .max_queued_per_query = 16,
                                                                     .policy = BackendSchedulingPolicy::RoundRobin,
                                                                     .priority_aging_ms = 0});
            OperationGate operations;
            std::vector<std::future<void>> tasks;
            // Release blocked operations before async futures join on every exit path.
            const ReleaseGateOnExit release_on_exit(operations);
            for (std::uint64_t query = 1; query <= 8; ++query) {
                tasks.push_back(std::async(std::launch::async, [&, query] {
                    scheduler.run(
                            BackendRequestScheduling{.query_id = query, .max_in_flight = 8, .priority = 0, .weight = 1},
                            "test", [&] { operations.enter_and_wait(); });
                }));
            }

            ASSERT_TRUE(operations.wait_for_entries(2));
            ASSERT_TRUE(wait_for_queue_size(scheduler, 6));
            EXPECT_EQ(scheduler.snapshot().in_flight, 2U);
            EXPECT_EQ(operations.maximum_active(), 2U);
            operations.release();
            ASSERT_TRUE(finish_tasks(tasks));

            EXPECT_EQ(operations.maximum_active(), 2U);
            EXPECT_EQ(scheduler.snapshot().in_flight, 0U);
            EXPECT_EQ(scheduler.snapshot().queued, 0U);
        }

        TEST(BackendRequestSchedulerTest, HonorsPerQueryConcurrencyLimit) {
            BackendRequestScheduler scheduler("test", BackendSchedulerConfig{.enabled = true,
                                                                             .max_in_flight = 4,
                                                                             .max_queued = 32,
                                                                             .max_queued_per_query = 16,
                                                                             .policy = BackendSchedulingPolicy::Fifo,
                                                                             .priority_aging_ms = 0});
            OperationGate operations;
            std::vector<std::future<void>> tasks;
            const ReleaseGateOnExit release_on_exit(operations);
            tasks.reserve(6);
            for (int index = 0; index < 6; ++index) {
                tasks.push_back(std::async(std::launch::async, [&] {
                    scheduler.run(
                            BackendRequestScheduling{.query_id = 7, .max_in_flight = 1, .priority = 0, .weight = 1},
                            "test", [&] { operations.enter_and_wait(); });
                }));
            }

            ASSERT_TRUE(operations.wait_for_entries(1));
            ASSERT_TRUE(wait_for_queue_size(scheduler, 5));
            EXPECT_EQ(scheduler.snapshot().in_flight, 1U);
            operations.release();
            ASSERT_TRUE(finish_tasks(tasks));

            EXPECT_EQ(operations.maximum_active(), 1U);
            EXPECT_EQ(scheduler.snapshot().in_flight, 0U);
        }

        TEST(BackendRequestSchedulerTest, PriorityPolicyChoosesHigherPriorityFromQueue) {
            BackendRequestScheduler scheduler("test",
                                              BackendSchedulerConfig{.enabled = true,
                                                                     .max_in_flight = 1,
                                                                     .max_queued = 16,
                                                                     .max_queued_per_query = 8,
                                                                     .policy = BackendSchedulingPolicy::Priority,
                                                                     .priority_aging_ms = 0});
            OperationGate blocker;
            std::mutex order_mutex;
            std::vector<int> order;
            std::vector<std::future<void>> tasks;
            const ReleaseGateOnExit release_on_exit(blocker);
            tasks.push_back(std::async(std::launch::async, [&] {
                scheduler.run(BackendRequestScheduling{.query_id = 1, .max_in_flight = 1, .priority = 0, .weight = 1},
                              "blocker", [&] { blocker.enter_and_wait(); });
            }));
            ASSERT_TRUE(blocker.wait_for_entries(1));

            for (const int priority: {-5, 10}) {
                tasks.push_back(std::async(std::launch::async, [&, priority] {
                    scheduler.run(BackendRequestScheduling{.query_id = static_cast<std::uint64_t>(priority + 10),
                                                           .max_in_flight = 1,
                                                           .priority = priority,
                                                           .weight = 1},
                                  "queued", [&] {
                                      std::scoped_lock lock(order_mutex);
                                      order.push_back(priority);
                                  });
                }));
            }

            ASSERT_TRUE(wait_for_queue_size(scheduler, 2));
            blocker.release();
            ASSERT_TRUE(finish_tasks(tasks));

            EXPECT_EQ(order, (std::vector<int>{10, -5}));
        }

        TEST(BackendRequestSchedulerTest, DisabledSchedulerRunsWithoutAdmissionLimits) {
            BackendRequestScheduler scheduler("test", BackendSchedulerConfig{.enabled = false, .max_in_flight = 1});
            OperationGate operations;
            std::vector<std::future<void>> tasks;
            const ReleaseGateOnExit release_on_exit(operations);
            tasks.reserve(2);
            for (int index = 0; index < 2; ++index) {
                tasks.push_back(std::async(std::launch::async, [&] {
                    scheduler.run(BackendRequestScheduling{.query_id = 1, .max_in_flight = 1}, "direct",
                                  [&] { operations.enter_and_wait(); });
                }));
            }

            ASSERT_TRUE(operations.wait_for_entries(2));
            EXPECT_EQ(operations.maximum_active(), 2U);
            EXPECT_EQ(scheduler.snapshot().in_flight, 0U);
            EXPECT_EQ(scheduler.snapshot().queued, 0U);
            operations.release();
            ASSERT_TRUE(finish_tasks(tasks));
        }

        TEST(BackendRequestSchedulerTest, ParsesAllPolicies) {
            EXPECT_EQ(parse_backend_scheduling_policy("fifo"), BackendSchedulingPolicy::Fifo);
            EXPECT_EQ(parse_backend_scheduling_policy("round-robin"), BackendSchedulingPolicy::RoundRobin);
            EXPECT_EQ(parse_backend_scheduling_policy("priority"), BackendSchedulingPolicy::Priority);
            EXPECT_EQ(parse_backend_scheduling_policy("weighted-fair"), BackendSchedulingPolicy::WeightedFair);
            EXPECT_EQ(parse_backend_scheduling_policy("priority-weighted-fair"),
                      BackendSchedulingPolicy::PriorityWeightedFair);
        }

    } // namespace
} // namespace polygraph
