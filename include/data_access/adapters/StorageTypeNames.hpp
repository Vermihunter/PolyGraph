#pragma once

#include <stdexcept>
#include <string>
#include <unordered_map>
#include "domain/graph/LogicalGraphSchema.hpp"
#include "utilities/StorageNames.hpp"

namespace polygraph {
    // Physical Neo4j labels and ArangoDB collections use the existing ingestion spelling.
    // Logical type properties and remote-type proxy metadata retain the original names.
    class StorageTypeNames {
        struct Names {
            std::unordered_map<TypeId, std::string> physical;
            std::unordered_map<std::string, TypeId> aliases;
            void add(TypeId id, const std::string &logical) {
                const auto name = storage_type_name(logical);
                for (const auto &alias: {logical, name}) {
                    const auto [it, inserted] = aliases.emplace(alias, id);
                    if (!inserted && it->second != id)
                        throw std::invalid_argument("Ambiguous storage type name: " + alias);
                }
                physical.emplace(id, name);
            }
        };
        Names nodes_;
        Names edges_;

    public:
        explicit StorageTypeNames(const domain::LogicalGraphSchema &schema) {
            for (const auto &[id, node]: schema.node_types())
                nodes_.add(id, node.name);
            for (const auto &[id, edge]: schema.edge_types())
                edges_.add(id, edge.name);
        }
        [[nodiscard]] const std::string &node_name(TypeId id) const { return nodes_.physical.at(id); }
        [[nodiscard]] const std::string &edge_name(TypeId id) const { return edges_.physical.at(id); }
        [[nodiscard]] TypeId node_type(const std::string &name) const { return nodes_.aliases.at(name); }
        [[nodiscard]] TypeId edge_type(const std::string &name) const { return edges_.aliases.at(name); }
        void validate_shared_namespace() const {
            for (const auto &[id, name]: edges_.physical)
                if (nodes_.aliases.contains(name))
                    throw std::invalid_argument("Node and edge share storage collection: " + name);
        }
    };
} // namespace polygraph
