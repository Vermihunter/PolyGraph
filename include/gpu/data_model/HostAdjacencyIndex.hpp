//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_HOSTADJACENCYINDEX_HPP
#define CPGDB_HOSTADJACENCYINDEX_HPP

#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include <boost/container_hash/hash.hpp>

#include "algorithms/ExpandSpec.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "data_model/EdgeRef.hpp"
#include "data_model/NeighborRef.hpp"
#include "data_model/NodeRef.hpp"

class HostAdjacencyIndex {
public:
    void add_edge(const NodeRef& src,
                  const NodeRef& dst,
                  const EdgeRef& edge) {
        nodes_[src.global_id] = src;
        nodes_[dst.global_id] = dst;

        EdgeRef stored_edge = edge;
        stored_edge.from = src.global_id;
        stored_edge.to = dst.global_id;

        outgoing_[src.global_id].push_back(NeighborRef{
            .vertex = dst,
            .via_edge = stored_edge
        });

        incoming_[dst.global_id].push_back(NeighborRef{
            .vertex = src,
            .via_edge = stored_edge
        });
    }

    [[nodiscard]] std::optional<NodeRef> node_ref(const GlobalId& id) const {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] std::optional<NeighborRef> find_neighbor(
        const GlobalId& source_id,
        const GlobalId& destination_id,
        const ExpandSpec& spec) const {

        if (auto found = find_matching_neighbor(
                outgoing_,
                source_id,
                destination_id,
                spec)) {
            return found;
        }

        if (spec.direction != Direction::Directed) {
            return find_matching_neighbor(
                incoming_,
                source_id,
                destination_id,
                spec);
        }

        return std::nullopt;
    }

    std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec) const {
        std::vector<AdjacencyRow> rows;
        rows.reserve(frontier.size());

        for (const auto& source : frontier) {
            AdjacencyRow row;
            row.source = source;

            std::unordered_set<GlobalId, boost::hash<GlobalId>> seen;

            append_matching_neighbors(row, seen, outgoing_, source.global_id, spec);

            if (spec.direction != Direction::Directed) {
                append_matching_neighbors(row, seen, incoming_, source.global_id, spec);
            }

            rows.push_back(std::move(row));
        }

        return rows;
    }

private:
    static bool neighbor_matches(
        const NeighborRef& neighbor,
        const GlobalId& destination_id,
        const ExpandSpec& spec) {

        if (neighbor.vertex.global_id != destination_id) {
            return false;
        }

        if (neighbor.via_edge.has_value() &&
            !spec.allows_edge_type(neighbor.via_edge->type)) {
            return false;
        }

        return spec.allows_target_type(neighbor.vertex.type);
    }

    static std::optional<NeighborRef> find_matching_neighbor(
        const std::unordered_map<GlobalId, std::vector<NeighborRef>, boost::hash<GlobalId>>& index,
        const GlobalId& source_id,
        const GlobalId& destination_id,
        const ExpandSpec& spec) {

        auto it = index.find(source_id);
        if (it == index.end()) {
            return std::nullopt;
        }

        for (const auto& neighbor : it->second) {
            if (neighbor_matches(neighbor, destination_id, spec)) {
                return neighbor;
            }
        }

        return std::nullopt;
    }

    static void append_matching_neighbors(
        AdjacencyRow& row,
        std::unordered_set<GlobalId, boost::hash<GlobalId>>& seen,
        const std::unordered_map<GlobalId, std::vector<NeighborRef>, boost::hash<GlobalId>>& index,
        const GlobalId& source_id,
        const ExpandSpec& spec) {

        auto it = index.find(source_id);
        if (it == index.end()) {
            return;
        }

        for (const auto& neighbor : it->second) {
            if (neighbor.via_edge.has_value() &&
                !spec.allows_edge_type(neighbor.via_edge->type)) {
                continue;
            }

            if (!spec.allows_target_type(neighbor.vertex.type)) {
                continue;
            }

            if (!seen.insert(neighbor.vertex.global_id).second) {
                continue;
            }

            row.neighbors.push_back(neighbor);
        }
    }

    std::unordered_map<GlobalId, NodeRef, boost::hash<GlobalId>> nodes_;
    std::unordered_map<GlobalId, std::vector<NeighborRef>, boost::hash<GlobalId>> outgoing_;
    std::unordered_map<GlobalId, std::vector<NeighborRef>, boost::hash<GlobalId>> incoming_;
};

#endif //CPGDB_HOSTADJACENCYINDEX_HPP
