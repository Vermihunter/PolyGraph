//
// Created by akos on 12. 7. 2026.
//

#include "logging/sinks/TextEventSerializer.hpp"

namespace cpgdb {
    namespace {
        std::string json_object_to_string(const nlohmann::json &obj) {
            std::string result;

            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (!result.empty())
                    result += ", ";

                result += it.key() + ": ";
                result += it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            }

            return result;
        }
    } // namespace

    std::string TextEventSerializer::serialize(EventLevel _level, std::string_view name, json fields) const {
        return std::string(name) + " - " + json_object_to_string(fields);
    }

} // namespace cpgdb
