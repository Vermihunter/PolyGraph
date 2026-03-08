#ifndef NODE_HPP
#define NODE_HPP

#include <vector>
#include <memory>
#include <unordered_map>

#include <data_model/EntityId.hpp>
//#include <data_model/Value.hpp>

struct Value;

struct Node {
  EntityId id;
  std::vector<std::string> labels;
  std::unordered_map<std::string, std::unique_ptr<Value>> properties;
};

#endif // NODE_HPP