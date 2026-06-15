//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_GRAPH_HPP
#define CPGDB_GRAPH_HPP

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "NodeType.hpp"
#include "EdgeType.hpp"
#include "utilities/json.hpp"

namespace domain {
    class LogicalGraphSchema {
    public:
        TypeId add_node_type(std::string name,
                             std::vector<Property> properties = {}) {
            TypeId id = next_type_id_++;

            NodeType type {
                .id = id,
                .name = std::move(name),
                .properties = std::move(properties)
            };

            node_name_to_id_[type.name] = id;
            node_types_[id] = std::move(type);

            return id;
        }

        TypeId add_edge_type(std::string name,
                             TypeId from,
                             TypeId to,
                             bool directed = true,
                             std::vector<Property> properties = {}) {
            TypeId id = next_type_id_++;

            EdgeType type {
                .type = id,
                .from = from,
                .to = to,
                .direction = directed ? Direction::Directed : Direction::Undirected,
                .name = std::move(name),
                .properties = std::move(properties)
            };

            edge_name_to_id_[type.name] = id;
            edge_types_[id] = std::move(type);

            return id;
        }

        const NodeType& node_type(TypeId id) const {
            return node_types_.at(id);
        }

        const EdgeType& edge_type(TypeId id) const {
            return edge_types_.at(id);
        }

        TypeId node_type_id(const std::string& name) const {
            auto it = node_name_to_id_.find(name);

            if (it != node_name_to_id_.end()) {
                return it->second;
            }

            std::string known;
            for (const auto& [known_name, id] : node_name_to_id_) {
                if (!known.empty()) {
                    known += ", ";
                }
                known += known_name;
            }

            throw std::runtime_error(
                "node_type_id(): unknown node type name='" +
                printable_string(name) +
                "', size=" +
                std::to_string(name.size()) +
                ". Known node types: [" +
                known +
                "]"
            );
        }

        TypeId edge_type_id(const std::string& name) const {
            auto it = edge_name_to_id_.find(name);

            if (it != edge_name_to_id_.end()) {
                return it->second;
            }

            std::string known;
            for (const auto& [known_name, id] : edge_name_to_id_) {
                if (!known.empty()) {
                    known += ", ";
                }
                known += known_name;
            }

            throw std::runtime_error(
                "edge_type_id(): unknown edge type name='" +
                printable_string(name) +
                "', size=" +
                std::to_string(name.size()) +
                ". Known edge types: [" +
                known +
                "]"
            );
        }

        const std::unordered_map<TypeId, NodeType>& node_types() const {
            return node_types_;
        }

        const std::unordered_map<TypeId, EdgeType>& edge_types() const {
            return edge_types_;
        }

    private:
        static std::string printable_string(const std::string& value) {
            std::string out;
            out.reserve(value.size());

            for (unsigned char c : value) {
                if (c == '\0') {
                    out += "\\0";
                } else {
                    out.push_back(static_cast<char>(c));
                }
            }

            return out;
        }

        TypeId next_type_id_ = 1;

        std::unordered_map<TypeId, NodeType> node_types_;
        std::unordered_map<TypeId, EdgeType> edge_types_;

        std::unordered_map<std::string, TypeId> node_name_to_id_;
        std::unordered_map<std::string, TypeId> edge_name_to_id_;
    };

    using graph_t = std::unique_ptr<LogicalGraphSchema>;
}

#endif // CPGDB_GRAPH_HPP
