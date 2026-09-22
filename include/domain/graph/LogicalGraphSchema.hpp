/**
 * @file include/domain/graph/LogicalGraphSchema.hpp
 * @brief Declarations for logical graph schema in the logical graph schema module.
 */

#ifndef POLYGRAPH_DOMAIN_GRAPH_LOGICAL_GRAPH_SCHEMA_HPP
#define POLYGRAPH_DOMAIN_GRAPH_LOGICAL_GRAPH_SCHEMA_HPP

#include "domain/graph/EdgeType.hpp"
#include "domain/graph/NodeType.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace polygraph::domain {

class LogicalGraphSchema {
  public:
    TypeId add_node_type(std::string name, std::vector<Property> properties = {});

    TypeId add_edge_type(std::string name,
                         TypeId from,
                         TypeId to,
                         bool directed = true,
                         std::vector<Property> properties = {});

    [[nodiscard]] const NodeType& node_type(TypeId id) const;
    [[nodiscard]] const EdgeType& edge_type(TypeId id) const;
    [[nodiscard]] TypeId node_type_id(const std::string& name) const;
    [[nodiscard]] TypeId edge_type_id(const std::string& name) const;

    [[nodiscard]] const std::unordered_map<TypeId, NodeType>& node_types() const noexcept;
    [[nodiscard]] const std::unordered_map<TypeId, EdgeType>& edge_types() const noexcept;

  private:
    [[nodiscard]] static std::string printable_string(const std::string& value);

    TypeId next_type_id_ = 1;
    std::unordered_map<TypeId, NodeType> node_types_;
    std::unordered_map<TypeId, EdgeType> edge_types_;
    std::unordered_map<std::string, TypeId> node_name_to_id_;
    std::unordered_map<std::string, TypeId> edge_name_to_id_;
};

using graph_t = std::unique_ptr<LogicalGraphSchema>;

} // namespace polygraph::domain

#endif // POLYGRAPH_DOMAIN_GRAPH_LOGICAL_GRAPH_SCHEMA_HPP
