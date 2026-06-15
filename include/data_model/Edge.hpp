#ifndef EDGE_HPP
#define EDGE_HPP

#include <vector>
#include <memory>
#include <unordered_map>

#include <data_model/EntityId.hpp>

#include "domain/graph/TypeId.hpp"
//#include <data_model/Value.hpp>

struct Value;

struct Edge {
  //EntityId id;

  /**
   * The unique global identifier of the concrete node that is unique across
   * the whole system
   */
  GlobalId global_id;

  /**
   * Represents the ID of the type of the node i.e. references @ref domain::NodeType
   */
  TypeId type;
  // std::string type;
  GlobalId src;
  GlobalId dst;
  std::unordered_map<std::string, std::unique_ptr<Value>> properties;
};

#endif // EDGE_HPP