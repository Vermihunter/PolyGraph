/**
 * @file src/infrastructure/scheduling/BackendRequestScheduler.cpp
 * @brief Implementation of server-global backend request scheduling.
 */
#include "infrastructure/scheduling/BackendRequestScheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace polygraph {

    namespace {
        std::atomic_uint64_t next_query_id{1};
    }

    std::uint64_t next_backend_scheduler_query_id() noexcept {
        return next_query_id.fetch_add(1, std::memory_order_relaxed);
    }

    std::string_view to_string(const BackendSchedulingPolicy policy) noexcept {
        switch (policy) {
            case BackendSchedulingPolicy::Fifo:
                return "fifo";
            case BackendSchedulingPolicy::RoundRobin:
                return "round-robin";
            case BackendSchedulingPolicy::Priority:
                return "priority";
            case BackendSchedulingPolicy::WeightedFair:
                return "weighted-fair";
            case BackendSchedulingPolicy::PriorityWeightedFair:
                return "priority-weighted-fair";
        }
        return "priority-weighted-fair";
    }

    BackendSchedulingPolicy parse_backend_scheduling_policy(const std::string_view value) {
        if (value == "fifo") {
            return BackendSchedulingPolicy::Fifo;
        }
        if (value == "round-robin") {
            return BackendSchedulingPolicy::RoundRobin;
        }
        if (value == "priority") {
            return BackendSchedulingPolicy::Priority;
        }
        if (value == "weighted-fair") {
            return BackendSchedulingPolicy::WeightedFair;
        }
        if (value == "priority-weighted-fair") {
            return BackendSchedulingPolicy::PriorityWeightedFair;
        }
        throw std::invalid_argument("Backend scheduler policy must be fifo, round-robin, priority, weighted-fair, or "
                                    "priority-weighted-fair");
    }

    struct BackendRequestScheduler::Impl {
        using Clock = std::chrono::steady_clock;

        struct Waiter {
            std::uint64_t sequence = 0;
            BackendRequestScheduling scheduling;
            Clock::time_point enqueued_at;
            bool granted = false;
        };

        struct QueryState {
            std::uint64_t order = 0;
            std::size_t queued = 0;
            std::size_t in_flight = 0;
            std::size_t producers_waiting = 0;
            std::size_t weight = 1;
            std::int64_t fair_score = 0;
        };

        explicit Impl(const BackendSchedulerConfig &scheduler_config) : config(scheduler_config) {}

        BackendSchedulerConfig config;
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::list<std::shared_ptr<Waiter>> waiters;
        std::unordered_map<std::uint64_t, QueryState> queries;
        std::size_t in_flight = 0;
        std::size_t queued = 0;
        std::uint64_t next_sequence = 1;
        std::uint64_t next_query_order = 1;
        std::uint64_t round_robin_cursor = 0;

        [[nodiscard]] bool global_queue_has_room() const noexcept {
            return config.max_queued == 0 || queued < config.max_queued;
        }

        [[nodiscard]] bool query_queue_has_room(const QueryState &query) const noexcept {
            return config.max_queued_per_query == 0 || query.queued < config.max_queued_per_query;
        }

        [[nodiscard]] bool eligible(const Waiter &waiter) const {
            const auto iterator = queries.find(waiter.scheduling.query_id);
            if (iterator == queries.end()) {
                return false;
            }
            return iterator->second.in_flight < waiter.scheduling.max_in_flight;
        }

        [[nodiscard]] std::int64_t effective_priority(const Waiter &waiter, const Clock::time_point now) const {
            std::int64_t result = waiter.scheduling.priority;
            if (config.priority_aging_ms != 0) {
                const auto waited_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - waiter.enqueued_at).count();
                result += waited_ms / static_cast<std::int64_t>(config.priority_aging_ms);
            }
            return result;
        }

        [[nodiscard]] std::vector<std::shared_ptr<Waiter>> oldest_eligible_per_query() const {
            std::unordered_map<std::uint64_t, std::shared_ptr<Waiter>> oldest;
            for (const auto &waiter: waiters) {
                if (!eligible(*waiter)) {
                    continue;
                }
                oldest.try_emplace(waiter->scheduling.query_id, waiter);
            }
            std::vector<std::shared_ptr<Waiter>> result;
            result.reserve(oldest.size());
            for (auto &[query_id, waiter]: oldest) {
                (void) query_id;
                result.push_back(std::move(waiter));
            }
            return result;
        }

        [[nodiscard]] std::shared_ptr<Waiter> choose_fifo() const {
            const auto iterator = std::find_if(waiters.begin(), waiters.end(),
                                               [this](const auto &waiter) { return eligible(*waiter); });
            return iterator == waiters.end() ? nullptr : *iterator;
        }

        [[nodiscard]] std::shared_ptr<Waiter> choose_round_robin() {
            const auto candidates = oldest_eligible_per_query();
            if (candidates.empty()) {
                return nullptr;
            }

            std::shared_ptr<Waiter> wrapped;
            std::shared_ptr<Waiter> after_cursor;
            for (const auto &waiter: candidates) {
                const auto query_iterator = queries.find(waiter->scheduling.query_id);
                if (query_iterator == queries.end()) {
                    continue;
                }
                const std::uint64_t order = query_iterator->second.order;
                if (!wrapped || order < queries.at(wrapped->scheduling.query_id).order) {
                    wrapped = waiter;
                }
                if (order > round_robin_cursor &&
                    (!after_cursor || order < queries.at(after_cursor->scheduling.query_id).order)) {
                    after_cursor = waiter;
                }
            }
            const auto selected = after_cursor ? after_cursor : wrapped;
            if (selected) {
                round_robin_cursor = queries.at(selected->scheduling.query_id).order;
            }
            return selected;
        }

        [[nodiscard]] std::shared_ptr<Waiter> choose_priority() const {
            const Clock::time_point now = Clock::now();
            std::shared_ptr<Waiter> selected;
            std::int64_t selected_priority = std::numeric_limits<std::int64_t>::min();
            for (const auto &waiter: waiters) {
                if (!eligible(*waiter)) {
                    continue;
                }
                const std::int64_t priority = effective_priority(*waiter, now);
                if (!selected || priority > selected_priority ||
                    (priority == selected_priority && waiter->sequence < selected->sequence)) {
                    selected = waiter;
                    selected_priority = priority;
                }
            }
            return selected;
        }

        [[nodiscard]] std::shared_ptr<Waiter>
        choose_weighted_fair(const std::optional<std::int64_t> required_priority = std::nullopt) {
            const Clock::time_point now = Clock::now();
            const auto candidates = oldest_eligible_per_query();
            if (candidates.empty()) {
                return nullptr;
            }

            std::int64_t total_weight = 0;
            std::shared_ptr<Waiter> selected;
            std::int64_t selected_score = std::numeric_limits<std::int64_t>::min();
            for (const auto &waiter: candidates) {
                if (required_priority && effective_priority(*waiter, now) != *required_priority) {
                    continue;
                }
                auto &query = queries.at(waiter->scheduling.query_id);
                query.weight = waiter->scheduling.weight;
                const auto weight = static_cast<std::int64_t>(query.weight);
                query.fair_score += weight;
                total_weight += weight;
                if (!selected || query.fair_score > selected_score ||
                    (query.fair_score == selected_score && waiter->sequence < selected->sequence)) {
                    selected = waiter;
                    selected_score = query.fair_score;
                }
            }
            if (selected && total_weight > 0) {
                queries.at(selected->scheduling.query_id).fair_score -= total_weight;
            }
            return selected;
        }

        [[nodiscard]] std::shared_ptr<Waiter> choose_priority_weighted_fair() {
            const Clock::time_point now = Clock::now();
            const auto candidates = oldest_eligible_per_query();
            if (candidates.empty()) {
                return nullptr;
            }
            std::int64_t highest = std::numeric_limits<std::int64_t>::min();
            for (const auto &waiter: candidates) {
                highest = std::max(highest, effective_priority(*waiter, now));
            }
            return choose_weighted_fair(highest);
        }

        [[nodiscard]] std::shared_ptr<Waiter> choose_waiter() {
            switch (config.policy) {
                case BackendSchedulingPolicy::Fifo:
                    return choose_fifo();
                case BackendSchedulingPolicy::RoundRobin:
                    return choose_round_robin();
                case BackendSchedulingPolicy::Priority:
                    return choose_priority();
                case BackendSchedulingPolicy::WeightedFair:
                    return choose_weighted_fair();
                case BackendSchedulingPolicy::PriorityWeightedFair:
                    return choose_priority_weighted_fair();
            }
            return nullptr;
        }

        void admit_locked() {
            while (in_flight < config.max_in_flight) {
                const auto waiter = choose_waiter();
                if (!waiter) {
                    break;
                }
                const auto iterator = std::find(waiters.begin(), waiters.end(), waiter);
                if (iterator == waiters.end()) {
                    break;
                }

                auto &query = queries.at(waiter->scheduling.query_id);
                --query.queued;
                ++query.in_flight;
                --queued;
                ++in_flight;
                waiter->granted = true;
                waiters.erase(iterator);
            }
            cv.notify_all();
        }

        void cleanup_query_if_idle(const std::uint64_t query_id) {
            const auto iterator = queries.find(query_id);
            if (iterator != queries.end() && iterator->second.queued == 0 && iterator->second.in_flight == 0 &&
                iterator->second.producers_waiting == 0) {
                queries.erase(iterator);
            }
        }
    };

    BackendRequestScheduler::Lease::Lease(BackendRequestScheduler *owner, const std::uint64_t query_id,
                                          const std::chrono::nanoseconds queue_wait) noexcept :
        owner_(owner), query_id_(query_id), queue_wait_(queue_wait) {}

    BackendRequestScheduler::Lease::Lease(Lease &&other) noexcept :
        owner_(std::exchange(other.owner_, nullptr)), query_id_(other.query_id_), queue_wait_(other.queue_wait_) {}

    BackendRequestScheduler::Lease &BackendRequestScheduler::Lease::operator=(Lease &&other) noexcept {
        if (this != &other) {
            reset();
            owner_ = std::exchange(other.owner_, nullptr);
            query_id_ = other.query_id_;
            queue_wait_ = other.queue_wait_;
        }
        return *this;
    }

    BackendRequestScheduler::Lease::~Lease() { reset(); }

    void BackendRequestScheduler::Lease::reset() noexcept {
        if (owner_ != nullptr) {
            owner_->release(query_id_);
            owner_ = nullptr;
        }
    }

    BackendRequestScheduler::BackendRequestScheduler(std::string backend_name, BackendSchedulerConfig config) :
        backend_name_(std::move(backend_name)), config_(config), impl_(std::make_unique<Impl>(config_)) {
        if (backend_name_.empty()) {
            throw std::invalid_argument("Backend scheduler name must not be empty");
        }
        if (config_.max_in_flight == 0) {
            throw std::invalid_argument("Backend scheduler max_in_flight must be greater than zero");
        }
    }

    BackendRequestScheduler::~BackendRequestScheduler() = default;

    BackendRequestScheduler::Lease BackendRequestScheduler::acquire(const BackendRequestScheduling &scheduling) {
        if (scheduling.max_in_flight == 0) {
            throw std::invalid_argument("Per-query backend scheduler max_in_flight must be greater than zero");
        }
        if (scheduling.weight == 0) {
            throw std::invalid_argument("Backend scheduler weight must be greater than zero");
        }

        const auto started = Impl::Clock::now();
        std::unique_lock lock(impl_->mutex);
        auto [query_iterator, inserted] = impl_->queries.try_emplace(scheduling.query_id);
        if (inserted) {
            query_iterator->second.order = impl_->next_query_order++;
        }
        query_iterator->second.weight = scheduling.weight;
        ++query_iterator->second.producers_waiting;

        impl_->cv.wait(lock, [this, query_id = scheduling.query_id] {
            const auto iterator = impl_->queries.find(query_id);
            return iterator != impl_->queries.end() && impl_->global_queue_has_room() &&
                   impl_->query_queue_has_room(iterator->second);
        });

        auto current_query = impl_->queries.find(scheduling.query_id);
        if (current_query == impl_->queries.end()) {
            throw std::logic_error("Backend scheduler query state disappeared while waiting to enqueue");
        }
        --current_query->second.producers_waiting;
        auto waiter = std::make_shared<Impl::Waiter>(Impl::Waiter{.sequence = impl_->next_sequence++,
                                                                  .scheduling = scheduling,
                                                                  .enqueued_at = Impl::Clock::now(),
                                                                  .granted = false});
        impl_->waiters.push_back(waiter);
        ++current_query->second.queued;
        ++impl_->queued;

        EVENT_INFO("backend.scheduler.enqueue", {"backend", backend_name_}, {"query_execution_id", scheduling.query_id},
                   {"priority", scheduling.priority}, {"weight", scheduling.weight},
                   {"query_max_in_flight", scheduling.max_in_flight}, {"global_in_flight", impl_->in_flight},
                   {"queue_size", impl_->queued}, {"policy", std::string(to_string(config_.policy))});

        impl_->admit_locked();
        impl_->cv.wait(lock, [&waiter] { return waiter->granted; });
        const auto waited = Impl::Clock::now() - started;

        EVENT_INFO("backend.scheduler.dequeue", {"backend", backend_name_}, {"query_execution_id", scheduling.query_id},
                   {"queue_wait_ms", std::chrono::duration<double, std::milli>(waited).count()},
                   {"global_in_flight", impl_->in_flight}, {"queue_size", impl_->queued});

        return Lease{this, scheduling.query_id, waited};
    }

    void BackendRequestScheduler::release(const std::uint64_t query_id) noexcept {
        try {
            std::scoped_lock lock(impl_->mutex);
            const auto iterator = impl_->queries.find(query_id);
            if (iterator == impl_->queries.end() || iterator->second.in_flight == 0 || impl_->in_flight == 0) {
                return;
            }
            --iterator->second.in_flight;
            --impl_->in_flight;
            EVENT_INFO("backend.scheduler.release", {"backend", backend_name_}, {"query_execution_id", query_id},
                       {"global_in_flight", impl_->in_flight}, {"queue_size", impl_->queued});
            impl_->cleanup_query_if_idle(query_id);
            impl_->admit_locked();
        } catch (...) {
            // Destructors and lease release must never throw.
        }
    }

    BackendSchedulerSnapshot BackendRequestScheduler::snapshot() const {
        std::scoped_lock lock(impl_->mutex);
        return BackendSchedulerSnapshot{.in_flight = impl_->in_flight,
                                        .queued = impl_->queued,
                                        .active_queries = impl_->queries.size(),
                                        .max_in_flight = config_.max_in_flight};
    }

} // namespace polygraph
