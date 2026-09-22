/**
 * @file include/data_model/Edge.hpp
 * @brief Declarations for edge in the graph data-model module.
 */

#pragma once

#include "data_model/EntityId.hpp"
#include "domain/graph/TypeId.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace polygraph {

struct Value;

struct Edge {
    GlobalId global_id;
    TypeId type;
    GlobalId src;
    GlobalId dst;
    std::unordered_map<std::string, std::unique_ptr<Value>> properties;
};

} // namespace polygraph
