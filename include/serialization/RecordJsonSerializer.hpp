/**
 * @file include/serialization/RecordJsonSerializer.hpp
 * @brief Declarations for record json serializer in the serialization module.
 */

#ifndef CPGDB_SERIALIZATION_RECORD_JSON_SERIALIZER_HPP
#define CPGDB_SERIALIZATION_RECORD_JSON_SERIALIZER_HPP

#include <data_model/Record.hpp>
#include <data_model/Value.hpp>
#include <memory>
#include <utilities/json.hpp>

namespace nlohmann {

template <typename T> struct adl_serializer<std::unique_ptr<T>> {
    static void to_json(json& value, const std::unique_ptr<T>& pointer) {
        value = pointer ? json(*pointer) : json(nullptr);
    }

    static void from_json(const json& value, std::unique_ptr<T>& pointer) {
        pointer = value.is_null() ? nullptr : std::make_unique<T>(value.get<T>());
    }
};

} // namespace nlohmann

namespace cpgdb {

void to_json(json& value, const Node& node);
void from_json(const json& value, Node& node);

void to_json(json& value, const Edge& edge);
void from_json(const json& value, Edge& edge);

void to_json(json& value, const Path& path);
void from_json(const json& value, Path& path);

void to_json(json& value, const Value& source);
void from_json(const json& value, Value& target);

void to_json(json& value, const Record& record);
void from_json(const json& value, Record& record);

} // namespace cpgdb

#endif // CPGDB_SERIALIZATION_RECORD_JSON_SERIALIZER_HPP
