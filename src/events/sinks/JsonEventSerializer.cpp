#include "events/sinks/JsonEventSerializer.hpp"

namespace polygraph::events {
    std::string JsonEventSerializer::serialize(EventLevel, std::string_view, const nlohmann::json &fields) const {
        return fields.dump();
    }
} // namespace polygraph::events
