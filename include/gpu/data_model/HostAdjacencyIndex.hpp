/**
 * @file include/gpu/data_model/HostAdjacencyIndex.hpp
 * @brief Declarations for host adjacency index in the GPU acceleration module.
 */

#pragma once

#include "algorithms/ExpandSpec.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "data_model/EdgeRef.hpp"
#include "data_model/NeighborRef.hpp"
#include "data_model/NodeRef.hpp"

#include <boost/container_hash/hash.hpp>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cpgdb {

class HostAdjacencyIndex {
  public:
    void add_edge(const NodeRef& source, const NodeRef& destination, const EdgeRef& edge);

    [[nodiscard]] std::optional<NeighborRef> find_neighbor(const GlobalId& source_id,
                                                           const GlobalId& destination_id,
                                                           const ExpandSpec& spec) const;

    [[nodiscard]] std::vector<AdjacencyRow> get_neighbors_batch(std::span<const NodeRef> frontier,
                                                                const ExpandSpec& spec) const;

  private:
    using NeighborIndex =
        std::unordered_map<GlobalId, std::vector<NeighborRef>, boost::hash<GlobalId>>;

    static bool neighbor_matches(const NeighborRef& neighbor,
                                 const GlobalId& destination_id,
                                 const ExpandSpec& spec);
    static std::optional<NeighborRef> find_matching_neighbor(const NeighborIndex& index,
                                                             const GlobalId& source_id,
                                                             const GlobalId& destination_id,
                                                             const ExpandSpec& spec);
    static void append_matching_neighbors(AdjacencyRow& row,
                                          std::unordered_set<GlobalId, boost::hash<GlobalId>>& seen,
                                          const NeighborIndex& index,
                                          const GlobalId& source_id,
                                          const ExpandSpec& spec);

    std::unordered_map<GlobalId, NodeRef, boost::hash<GlobalId>> nodes_;
    NeighborIndex outgoing_;
    NeighborIndex incoming_;
};

} // namespace cpgdb
