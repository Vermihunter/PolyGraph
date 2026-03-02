#ifndef EDGE_HPP
#define EDGE_HPP

#include <vector>
#include <unordered_map>

#include <data_model/EntityId.hpp>
#include <data_model/Value.hpp>

struct Edge {
  EntityId id;
  std::string type;
  EntityId src;
  EntityId dst;
  std::unordered_map<std::string, Value> properties;
};

#endif // EDGE_HPP