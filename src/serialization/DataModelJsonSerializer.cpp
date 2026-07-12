/**
 * @file src/serialization/DataModelJsonSerializer.cpp
 * @brief Implementation for data model json serializer in the serialization module.
 */

#include "serialization/DataModelJsonSerializer.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace cpgdb {

    value_t DataModelJsonSerializer::json_primitive_to_value(const json &value) {
        if (value.is_number_unsigned()) {
            const auto unsigned_value = value.get<std::uint64_t>();
            if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::out_of_range("JSON unsigned integer does not fit in Value::integer_type");
            }
            return std::make_unique<Value>(static_cast<std::int64_t>(unsigned_value));
        }
        if (value.is_number_integer()) {
            return std::make_unique<Value>(value.get<std::int64_t>());
        }
        if (value.is_boolean()) {
            return std::make_unique<Value>(value.get<bool>());
        }
        if (value.is_number_float()) {
            return std::make_unique<Value>(value.get<double>());
        }
        if (value.is_string()) {
            return std::make_unique<Value>(value.get<std::string>());
        }
        return nullptr;
    }

} // namespace cpgdb
