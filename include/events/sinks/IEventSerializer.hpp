#pragma once

#include "events/EventLevel.hpp"
#include "utilities/json.hpp"

#include <string>
#include <string_view>

namespace polygraph::events {

class IEventSerializer {
  public:
    virtual ~IEventSerializer() = default;
    [[nodiscard]] virtual std::string serialize(
        EventLevel level,
        std::string_view event_name,
        const nlohmann::json& fields) const = 0;
};

} // namespace polygraph::events
