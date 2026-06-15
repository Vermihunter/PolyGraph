#ifndef NODE_HPP
#define NODE_HPP

#include <vector>
#include <memory>
#include <unordered_map>

#include <data_model/EntityId.hpp>

#include "domain/graph/TypeId.hpp"
//#include <data_model/Value.hpp>

struct Value;

struct Node {
  /**
   * The unique global identifier of the concrete node that is unique across
   * the whole system
   */
  GlobalId global_id;

  /**
   * Represents the ID of the type of the node i.e. references @ref domain::NodeType
   */
  TypeId type;

  std::vector<std::string> labels;
  std::unordered_map<std::string, std::unique_ptr<Value>> properties;
};

using node_t = std::unique_ptr<Node>;

#endif // NODE_HPP