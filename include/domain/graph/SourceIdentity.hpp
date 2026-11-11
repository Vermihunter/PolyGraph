#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include "utilities/json.hpp"

namespace polygraph::domain {
    struct SourceIdentity {
        std::string name_space;
        std::string type;
        json value;
    };

    // Shared v1 encoding used by ingestion keys and runtime endpoint/root lookup.
    [[nodiscard]] inline std::string encode_source_identity(const SourceIdentity &identity, bool edge = false) {
        if (identity.name_space.empty() || identity.type.empty() ||
            identity.name_space.find('\0') != std::string::npos || identity.type.find('\0') != std::string::npos)
            throw std::invalid_argument("Source identity requires nonempty NUL-free namespace and logical type");
        const auto &value = identity.value;
        const auto kind = value.is_string()            ? "string"
                          : value.is_number_unsigned() ? "uint64"
                          : value.is_number_integer()  ? "int64"
                                                       : "invalid";
        if (std::string_view(kind) == "invalid")
            throw std::invalid_argument("Source identity ID must be a string, int64 or uint64 for " + identity.type);
        try {
            return json::array({"polygraph.source.v1", identity.name_space, edge ? "edge" : "vertex", identity.type,
                                kind, value})
                    .dump();
        } catch (const json::exception &e) {
            throw std::invalid_argument("Invalid source identity encoding: " + std::string(e.what()));
        }
    }

    // Runtime selector syntax uses an explicit idType so positive signed IDs remain exact.
    [[nodiscard]] inline SourceIdentity source_identity_from_json(const json &object) {
        if (!object.is_object() || object.size() != 4 || !object.contains("namespace") || !object.contains("type") ||
            !object.contains("idType") || !object.contains("id"))
            throw std::invalid_argument("Source selector requires exactly namespace, type, idType and id");
        SourceIdentity result{object.at("namespace").get<std::string>(), object.at("type").get<std::string>(),
                              object.at("id")};
        const auto kind = object.at("idType").get<std::string>();
        if (kind == "string") {
            if (!result.value.is_string())
                throw std::invalid_argument("string source ID requires a JSON string");
        } else if (kind == "int64") {
            if (!result.value.is_number_integer() ||
                (result.value.is_number_unsigned() &&
                 result.value.get<std::uint64_t>() >
                         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
                throw std::invalid_argument("int64 source ID requires an exact signed 64-bit JSON integer");
            result.value = json(result.value.get<std::int64_t>());
        } else if (kind == "uint64") {
            if (!result.value.is_number_integer() ||
                (result.value.is_number_integer() && !result.value.is_number_unsigned() &&
                 result.value.get<std::int64_t>() < 0))
                throw std::invalid_argument("uint64 source ID requires an exact nonnegative 64-bit JSON integer");
            result.value = json(result.value.get<std::uint64_t>());
        } else
            throw std::invalid_argument("Source idType must be string, int64 or uint64");
        (void) encode_source_identity(result);
        return result;
    }
} // namespace polygraph::domain
