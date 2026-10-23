#pragma once

#include "events/EventLevel.hpp"
#include "utilities/json.hpp"

#include <string_view>

namespace polygraph::events {

class IEventSink {
  public:
    virtual ~IEventSink() = default;
    [[nodiscard]] virtual bool accepts(EventLevel level, std::string_view event_name) const noexcept = 0;
    virtual void write(EventLevel level, std::string_view event_name, const nlohmann::json& fields) = 0;
    virtual void flush() = 0;
};

} // namespace polygraph::events
