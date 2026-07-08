//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_REQUESTPARSING_HPP
#define CPGDB_REQUESTPARSING_HPP


#include <drogon/drogon.h>

#include <optional>
#include <string>
#include <string_view>

#include "ErrorMapper.hpp"

namespace http::parse {

inline const Json::Value& jsonBody(const drogon::HttpRequestPtr& request) {
    const auto& json = request->getJsonObject();

    if (!json) {
        throw BadRequest("Expected request body with Content-Type: application/json");
    }

    if (!json->isObject()) {
        throw BadRequest("Expected JSON object body");
    }

    return *json;
}

inline bool hasField(const Json::Value& json, std::string_view field) {
    return json.isMember(std::string(field));
}

inline std::string requiredString(
    const Json::Value& json,
    std::string_view field
) {
    const std::string key(field);

    if (!json.isMember(key)) {
        throw BadRequest("Missing required field: " + key);
    }

    if (!json[key].isString()) {
        throw BadRequest("Field must be a string: " + key);
    }

    const auto value = json[key].asString();

    if (value.empty()) {
        throw BadRequest("Field must not be empty: " + key);
    }

    return value;
}

inline std::optional<std::string> optionalString(
    const Json::Value& json,
    std::string_view field
) {
    const std::string key(field);

    if (!json.isMember(key) || json[key].isNull()) {
        return std::nullopt;
    }

    if (!json[key].isString()) {
        throw BadRequest("Field must be a string: " + key);
    }

    return json[key].asString();
}

inline int requiredInt(
    const Json::Value& json,
    std::string_view field
) {
    const std::string key(field);

    if (!json.isMember(key)) {
        throw BadRequest("Missing required field: " + key);
    }

    if (!json[key].isInt()) {
        throw BadRequest("Field must be an integer: " + key);
    }

    return json[key].asInt();
}

inline int optionalInt(
    const Json::Value& json,
    std::string_view field,
    int fallback
) {
    const std::string key(field);

    if (!json.isMember(key) || json[key].isNull()) {
        return fallback;
    }

    if (!json[key].isInt()) {
        throw BadRequest("Field must be an integer: " + key);
    }

    return json[key].asInt();
}

inline int optionalIntInRange(
    const Json::Value& json,
    std::string_view field,
    int fallback,
    int minValue,
    int maxValue
) {
    const auto value = optionalInt(json, field, fallback);
    const std::string key(field);

    if (value < minValue || value > maxValue) {
        throw BadRequest(
            "Field out of range: " + key +
            " must be between " + std::to_string(minValue) +
            " and " + std::to_string(maxValue)
        );
    }

    return value;
}

inline bool optionalBool(
    const Json::Value& json,
    std::string_view field,
    bool fallback
) {
    const std::string key(field);

    if (!json.isMember(key) || json[key].isNull()) {
        return fallback;
    }

    if (!json[key].isBool()) {
        throw BadRequest("Field must be a boolean: " + key);
    }

    return json[key].asBool();
}

inline std::string requiredQueryParam(
    const drogon::HttpRequestPtr& request,
    std::string_view name
) {
    const std::string key(name);
    const auto value = request->getParameter(key);

    if (value.empty()) {
        throw BadRequest("Missing required query parameter: " + key);
    }

    return value;
}

inline std::optional<std::string> optionalQueryParam(
    const drogon::HttpRequestPtr& request,
    std::string_view name
) {
    const std::string key(name);
    auto value = request->getParameter(key);

    if (value.empty()) {
        return std::nullopt;
    }

    return value;
}

template <typename T>
inline T optionalQueryParamAs(
    const drogon::HttpRequestPtr& request,
    std::string_view name,
    T fallback
) {
    const std::string key(name);
    const auto parsed = request->getOptionalParameter<T>(key);

    if (!parsed.has_value()) {
        return fallback;
    }

    return *parsed;
}

inline std::string requiredHeader(
    const drogon::HttpRequestPtr& request,
    std::string_view name
) {
    const std::string key(name);
    const auto value = request->getHeader(key);

    if (value.empty()) {
        throw BadRequest("Missing required header: " + key);
    }

    return value;
}

} // namespace http::parse

#endif //CPGDB_REQUESTPARSING_HPP
