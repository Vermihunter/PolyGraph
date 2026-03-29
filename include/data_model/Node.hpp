#ifndef NODE_HPP
#define NODE_HPP

#include <vector>
#include <memory>
#include <unordered_map>

#include <data_model/EntityId.hpp>
//#include <data_model/Value.hpp>

struct Value;

struct Node {
  //Node() = default;
  //Node(const Node&) = default;

  EntityId id;
  GlobalId global_id;
  std::vector<std::string> labels;
  std::unordered_map<std::string, std::unique_ptr<Value>> properties;
};

using node_t = std::unique_ptr<Node>;

#endif // NODE_HPP