#ifndef EDGE_HPP
#define EDGE_HPP

#include <vector>
#include <memory>
#include <unordered_map>

#include <data_model/EntityId.hpp>
//#include <data_model/Value.hpp>

struct Value;

struct Edge {
  EntityId id;
  GlobalId global_id;
  std::string type;
  EntityId src;
  EntityId dst;
  std::unordered_map<std::string, std::unique_ptr<Value>> properties;
};

#endif // EDGE_HPP