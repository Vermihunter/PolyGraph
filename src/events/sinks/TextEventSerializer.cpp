#include "events/sinks/TextEventSerializer.hpp"

namespace polygraph::events {
    namespace {
        std::string fields_to_string(const nlohmann::json &object) {
            std::string result;
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (!result.empty()) {
                    result += ", ";
                }
                result += it.key() + ": ";
                result += it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            }
            return result;
        }
    } // namespace

    std::string TextEventSerializer::serialize(EventLevel, std::string_view name, const nlohmann::json &fields) const {
        return std::string{name} + " - " + fields_to_string(fields);
    }
} // namespace polygraph::events
