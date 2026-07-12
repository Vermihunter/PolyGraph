/**
 * @file src/gpu/data_model/HostAdjacencyIndex.cpp
 * @brief Implementation for host adjacency index in the GPU acceleration module.
 */

#include "gpu/data_model/HostAdjacencyIndex.hpp"

namespace cpgdb {

    void HostAdjacencyIndex::add_edge(const NodeRef &source, const NodeRef &destination, const EdgeRef &edge) {
        nodes_[source.global_id] = source;
        nodes_[destination.global_id] = destination;

        auto stored_edge = edge;
        stored_edge.from = source.global_id;
        stored_edge.to = destination.global_id;

        outgoing_[source.global_id].push_back({.vertex = destination, .via_edge = stored_edge});
        incoming_[destination.global_id].push_back({.vertex = source, .via_edge = stored_edge});
    }

    std::optional<NeighborRef> HostAdjacencyIndex::find_neighbor(const GlobalId &source_id,
                                                                 const GlobalId &destination_id,
                                                                 const ExpandSpec &spec) const {
        if (auto found = find_matching_neighbor(outgoing_, source_id, destination_id, spec)) {
            return found;
        }
        if (spec.direction != Direction::Directed) {
            return find_matching_neighbor(incoming_, source_id, destination_id, spec);
        }
        return std::nullopt;
    }

    std::vector<AdjacencyRow> HostAdjacencyIndex::get_neighbors_batch(const std::span<const NodeRef> frontier,
                                                                      const ExpandSpec &spec) const {
        std::vector<AdjacencyRow> rows;
        rows.reserve(frontier.size());

        for (const auto &source: frontier) {
            AdjacencyRow row{.source = source, .neighbors = {}};
            std::unordered_set<GlobalId, boost::hash<GlobalId>> seen;
            append_matching_neighbors(row, seen, outgoing_, source.global_id, spec);
            if (spec.direction != Direction::Directed) {
                append_matching_neighbors(row, seen, incoming_, source.global_id, spec);
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    bool HostAdjacencyIndex::neighbor_matches(const NeighborRef &neighbor, const GlobalId &destination_id,
                                              const ExpandSpec &spec) {
        if (neighbor.vertex.global_id != destination_id)
            return false;
        if (neighbor.via_edge && !spec.allows_edge_type(neighbor.via_edge->type))
            return false;
        return spec.allows_target_type(neighbor.vertex.type);
    }

    std::optional<NeighborRef> HostAdjacencyIndex::find_matching_neighbor(const NeighborIndex &index,
                                                                          const GlobalId &source_id,
                                                                          const GlobalId &destination_id,
                                                                          const ExpandSpec &spec) {
        const auto iterator = index.find(source_id);
        if (iterator == index.end())
            return std::nullopt;
        for (const auto &neighbor: iterator->second) {
            if (neighbor_matches(neighbor, destination_id, spec))
                return neighbor;
        }
        return std::nullopt;
    }

    void HostAdjacencyIndex::append_matching_neighbors(AdjacencyRow &row,
                                                       std::unordered_set<GlobalId, boost::hash<GlobalId>> &seen,
                                                       const NeighborIndex &index, const GlobalId &source_id,
                                                       const ExpandSpec &spec) {
        const auto iterator = index.find(source_id);
        if (iterator == index.end())
            return;

        for (const auto &neighbor: iterator->second) {
            if (neighbor.via_edge && !spec.allows_edge_type(neighbor.via_edge->type))
                continue;
            if (!spec.allows_target_type(neighbor.vertex.type))
                continue;
            if (!seen.insert(neighbor.vertex.global_id).second)
                continue;
            row.neighbors.push_back(neighbor);
        }
    }

} // namespace cpgdb
