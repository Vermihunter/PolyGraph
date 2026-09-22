/**
 * @file include/serialization/DataModelJsonSerializer.hpp
 * @brief Declarations for data model json serializer in the serialization module.
 */

#pragma once

#include "data_model/Value.hpp"
#include "utilities/json.hpp"

namespace polygraph {

class DataModelJsonSerializer {
  public:
    [[nodiscard]] static value_t json_primitive_to_value(const json& value);
};

} // namespace polygraph
