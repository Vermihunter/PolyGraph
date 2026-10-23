#pragma once
#include "events/sinks/IEventSerializer.hpp"

namespace polygraph::events {
class JsonEventSerializer final : public IEventSerializer {
  public:
    [[nodiscard]] std::string serialize(EventLevel level,
                                        std::string_view event_name,
                                        const nlohmann::json& fields) const override;
};
} // namespace polygraph::events
