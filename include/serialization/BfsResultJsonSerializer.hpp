/**
 * @file include/serialization/BfsResultJsonSerializer.hpp
 * @brief Declarations for bfs result json serializer in the serialization module.
 */

#pragma once

#include "algorithms/bfs/BfsResult.hpp"
#include "utilities/json.hpp"

namespace cpgdb {

void to_json(json& json, const BfsResult& result);

} // namespace cpgdb
