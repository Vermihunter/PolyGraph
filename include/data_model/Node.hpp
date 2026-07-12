/**
 * @file include/data_model/Node.hpp
 * @brief Declarations for node in the graph data-model module.
 */

#pragma once

#include "data_model/EntityId.hpp"
#include "domain/graph/TypeId.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpgdb {

struct Value;

struct Node {
    GlobalId global_id;
    TypeId type;
    std::vector<std::string> labels;
    std::unordered_map<std::string, std::unique_ptr<Value>> properties;
};

} // namespace cpgdb
