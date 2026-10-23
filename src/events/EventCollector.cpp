/**
 * @file src/events/EventCollector.cpp
 * @brief Structured execution-event collector implementation.
 */
#include "events/EventCollector.hpp"

#include "observability/MetricsAccess.hpp"

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>

namespace polygraph::events {

    EventCollector::EventCollector(std::string service, const EventLevel minimum_level,
                                   observability::EventSelection selection,
                                   std::vector<std::unique_ptr<IEventSink>> sinks) :
        service_(std::move(service)), minimum_level_(minimum_level), selection_(std::move(selection)),
        start_time_(std::chrono::steady_clock::now()), sinks_(std::move(sinks)) {}

    bool EventCollector::matches_prefixes(const std::string_view value,
                                          const std::vector<std::string> &prefixes) noexcept {
        return std::ranges::any_of(prefixes, [&](const auto &prefix) { return value.starts_with(prefix); });
    }

    bool EventCollector::accepts(const EventLevel level, const std::string_view event_name) const noexcept {
        if (minimum_level_ == EventLevel::Off || level < minimum_level_) {
            return false;
        }
        if (!selection_.include_prefixes.empty() && !matches_prefixes(event_name, selection_.include_prefixes)) {
            return false;
        }
        if (matches_prefixes(event_name, selection_.exclude_prefixes)) {
            return false;
        }

        return std::ranges::any_of(sinks_, [&](const auto &sink) { return sink->accepts(level, event_name); });
    }

    void EventCollector::emit(const EventLevel level, const std::string_view event_name, json fields) {
        if (!accepts(level, event_name)) {
            return;
        }

        const auto [elapsed_ns, unix_ns] = get_times();
        fields["sequence"] = sequence_.fetch_add(1, std::memory_order_relaxed);
        fields["event"] = event_name;
        fields["level"] = observability::to_string(level);
        fields["service"] = service_;
        fields["elapsed_ns"] = elapsed_ns;
        fields["timestamp_unix_ns"] = unix_ns;
        fields["thread_id"] = thread_id();

        observability::observe_event(level);

        for (auto &sink: sinks_) {
            if (sink->accepts(level, event_name)) {
                sink->write(level, event_name, fields);
            }
        }
    }

    void EventCollector::flush() {
        for (auto &sink: sinks_) {
            sink->flush();
        }
    }

    std::pair<std::uint64_t, std::uint64_t> EventCollector::get_times() const {
        const auto steady_now = std::chrono::steady_clock::now();
        const auto system_now = std::chrono::system_clock::now();
        return {
                static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(steady_now - start_time_).count()),
                static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(system_now.time_since_epoch()).count()),
        };
    }

    std::uint64_t EventCollector::thread_id() {
        return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

} // namespace polygraph::events
