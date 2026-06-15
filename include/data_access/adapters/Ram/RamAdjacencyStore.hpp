//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_RAMADJACENCYSTORE_HPP
#define CPGDB_RAMADJACENCYSTORE_HPP
#include "data_access/IGraphStore.hpp"

class RamAdjacencyStore final : public IGraphStore {
public:
    explicit RamAdjacencyStore(BackendDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}

    BackendDescriptor descriptor() const override {
        return descriptor_;
    }

    void put(const NodeRef& source, std::vector<NeighborRef> neighbors) {
        adjacency_[source.id] = CachedRow{
            .source = source,
            .neighbors = std::move(neighbors)
        };
    }

    std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec) override {
        std::vector<AdjacencyRow> rows;

        for (const auto& source : frontier) {
            auto it = adjacency_.find(source.id);

            if (it == adjacency_.end()) {
                continue;
            }

            AdjacencyRow row;
            row.source = source;

            for (const auto& neighbor : it->second.neighbors) {
                if (neighbor.via_edge.has_value() &&
                    !spec.allows_edge_type(neighbor.via_edge->type)) {
                    continue;
                    }

                if (!spec.allows_target_type(neighbor.vertex.type)) {
                    continue;
                }

                row.neighbors.push_back(neighbor);
            }

            rows.push_back(std::move(row));
        }

        return rows;
    }

private:
    struct CachedRow {
        NodeRef source;
        std::vector<NeighborRef> neighbors;
    };

private:
    BackendDescriptor descriptor_;
    std::unordered_map<GlobalId, CachedRow> adjacency_;
};

#endif //CPGDB_RAMADJACENCYSTORE_HPP