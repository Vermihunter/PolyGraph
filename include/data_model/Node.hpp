#ifndef NODE_HPP
#define NODE_HPP

#include <vector>
#include <unordered_map>

#include <data_model/EntityId.hpp>
#include <data_model/Value.hpp>

struct Node {
  EntityId id;
  std::vector<std::string> labels;
  std::unordered_map<std::string, Value> properties;
};

#endif // NODE_HPP