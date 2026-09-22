/**
 * @file src/domain/graph/LogicalGraphSchema.cpp
 * @brief Implementation for logical graph schema in the logical graph schema module.
 */

#include "domain/graph/LogicalGraphSchema.hpp"

#include <stdexcept>
#include <utility>

namespace polygraph::domain {

    TypeId LogicalGraphSchema::add_node_type(std::string name, std::vector<Property> properties) {
        const TypeId id = next_type_id_++;
        NodeType type{.id = id, .name = std::move(name), .properties = std::move(properties)};
        node_name_to_id_.insert_or_assign(type.name, id);
        node_types_.insert_or_assign(id, std::move(type));
        return id;
    }

    TypeId LogicalGraphSchema::add_edge_type(std::string name, TypeId from, TypeId to, bool directed,
                                             std::vector<Property> properties) {
        const TypeId id = next_type_id_++;
        EdgeType type{.type = id,
                      .from = from,
                      .to = to,
                      .direction = directed ? Direction::Directed : Direction::Undirected,
                      .name = std::move(name),
                      .properties = std::move(properties)};
        edge_name_to_id_.insert_or_assign(type.name, id);
        edge_types_.insert_or_assign(id, std::move(type));
        return id;
    }

    const NodeType &LogicalGraphSchema::node_type(TypeId id) const { return node_types_.at(id); }

    const EdgeType &LogicalGraphSchema::edge_type(TypeId id) const { return edge_types_.at(id); }

    TypeId LogicalGraphSchema::node_type_id(const std::string &name) const {
        if (const auto it = node_name_to_id_.find(name); it != node_name_to_id_.end()) {
            return it->second;
        }

        std::string known;
        for (const auto &[known_name, id]: node_name_to_id_) {
            (void) id;
            if (!known.empty()) {
                known += ", ";
            }
            known += known_name;
        }

        throw std::runtime_error("node_type_id(): unknown node type name='" + printable_string(name) +
                                 "', size=" + std::to_string(name.size()) + ". Known node types: [" + known + "]");
    }

    TypeId LogicalGraphSchema::edge_type_id(const std::string &name) const {
        if (const auto it = edge_name_to_id_.find(name); it != edge_name_to_id_.end()) {
            return it->second;
        }

        std::string known;
        for (const auto &[known_name, id]: edge_name_to_id_) {
            (void) id;
            if (!known.empty()) {
                known += ", ";
            }
            known += known_name;
        }

        throw std::runtime_error("edge_type_id(): unknown edge type name='" + printable_string(name) +
                                 "', size=" + std::to_string(name.size()) + ". Known edge types: [" + known + "]");
    }

    const std::unordered_map<TypeId, NodeType> &LogicalGraphSchema::node_types() const noexcept { return node_types_; }

    const std::unordered_map<TypeId, EdgeType> &LogicalGraphSchema::edge_types() const noexcept { return edge_types_; }

    std::string LogicalGraphSchema::printable_string(const std::string &value) {
        std::string output;
        output.reserve(value.size());
        for (const unsigned char character: value) {
            if (character == '\0') {
                output += "\\0";
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
        return output;
    }

} // namespace polygraph::domain
