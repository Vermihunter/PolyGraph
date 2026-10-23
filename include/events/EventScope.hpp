/**
 * @file include/events/EventScope.hpp
 * @brief RAII span that emits <name>.start and <name>.end execution events.
 */
#pragma once

#include "events/EventCollector.hpp"
#include "events/EventLevel.hpp"

#include <chrono>
#include <string>

namespace polygraph::events {

class EventScope final {
  public:
    using json = nlohmann::json;

    EventScope() noexcept = default;
    EventScope(EventCollector* collector, EventLevel level, std::string name, json fields);

    EventScope(const EventScope&) = delete;
    EventScope& operator=(const EventScope&) = delete;
    EventScope(EventScope&& other) noexcept;
    EventScope& operator=(EventScope&&) = delete;

    ~EventScope() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    void add_end_field(std::string key, json value);

  private:
    EventCollector* collector_{};
    EventLevel level_{EventLevel::Info};
    std::string name_;
    json fields_;
    json end_fields_;
    std::chrono::steady_clock::time_point start_{};
    bool active_{false};
};

} // namespace polygraph::events
