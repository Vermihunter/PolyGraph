/**
 * @file include/events/EventCollector.hpp
 * @brief Runtime destination for structured execution events.
 */
#pragma once

#include "events/EventLevel.hpp"
#include "events/sinks/IEventSink.hpp"
#include "observability/ObservabilityConfig.hpp"
#include "utilities/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace polygraph::events {

class EventCollector final {
  public:
    using json = nlohmann::json;

    EventCollector(std::string service,
                   EventLevel minimum_level,
                   observability::EventSelection selection,
                   std::vector<std::unique_ptr<IEventSink>> sinks);

    [[nodiscard]] bool accepts(EventLevel level, std::string_view event_name) const noexcept;
    void emit(EventLevel level, std::string_view event_name, json fields = json::object());
    void flush();

  private:
    [[nodiscard]] static bool matches_prefixes(
        std::string_view value,
        const std::vector<std::string>& prefixes) noexcept;
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> get_times() const;
    [[nodiscard]] static std::uint64_t thread_id();

    std::string service_;
    EventLevel minimum_level_;
    observability::EventSelection selection_;
    std::chrono::steady_clock::time_point start_time_;
    std::vector<std::unique_ptr<IEventSink>> sinks_;
    std::atomic<std::uint64_t> sequence_{1};
};

} // namespace polygraph::events
