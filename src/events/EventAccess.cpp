/**
 * @file src/events/EventAccess.cpp
 * @brief Process-wide execution-event collector access.
 */
#include "events/EventAccess.hpp"

#include <atomic>
#include <utility>

namespace polygraph::events {
    namespace {
        std::unique_ptr<EventCollector> collector_owner;
        std::atomic<EventCollector *> active_collector{nullptr};
    } // namespace

    EventCollector *collector() noexcept { return active_collector.load(std::memory_order_relaxed); }

    EventCollector *collector(const EventLevel level, const std::string_view event_name) noexcept {
        auto *current = active_collector.load(std::memory_order_relaxed);
        if (current == nullptr || !current->accepts(level, event_name)) {
            return nullptr;
        }
        return current;
    }

    void install(std::unique_ptr<EventCollector> new_collector) noexcept {
        collector_owner = std::move(new_collector);
        active_collector.store(collector_owner.get(), std::memory_order_release);
    }

    std::unique_ptr<EventCollector> release() noexcept {
        active_collector.store(nullptr, std::memory_order_release);
        return std::move(collector_owner);
    }

} // namespace polygraph::events
