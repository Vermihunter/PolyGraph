/**
 * @file include/logging/EventScope.hpp
 * @brief Declarations for event scope in the event logging module.
 */

#ifndef CPGDB_LOGGING_EVENT_SCOPE_HPP
#define CPGDB_LOGGING_EVENT_SCOPE_HPP

#include "logging/EventLevel.hpp"
#include "logging/EventLogger.hpp"

#include <chrono>
#include <string>

namespace cpgdb {

class EventScope {
  public:
    EventScope() noexcept = default;

    EventScope(EventLogger* logger, EventLevel level, std::string name, json fields);

    EventScope(EventLogger& logger, EventLevel level, std::string name, json fields);

    EventScope(const EventScope&) = delete;
    EventScope& operator=(const EventScope&) = delete;

    EventScope(EventScope&& other) noexcept;
    EventScope& operator=(EventScope&& other) = delete;

    ~EventScope() noexcept;

    void add_end_field(std::string key, json value);

  private:
    EventLogger* logger_{};
    EventLevel level_{EventLevel::Info};
    std::string name_;
    json fields_;
    json end_fields_;
    std::chrono::steady_clock::time_point start_{};
    bool active_{false};
};

} // namespace cpgdb

#endif // CPGDB_LOGGING_EVENT_SCOPE_HPP
