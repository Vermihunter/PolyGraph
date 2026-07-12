/**
 * @file include/algorithms/bfs/BfsLayer.hpp
 * @brief Streaming events emitted after each completed BFS depth.
 */

#ifndef CPGDB_BFSLAYER_HPP
#define CPGDB_BFSLAYER_HPP

#include "data_model/NodeRef.hpp"

#include <cstdint>
#include <vector>

namespace cpgdb {

/** @brief One predecessor-tree edge discovered while constructing a BFS layer. */
struct BfsTreeEdgeRef {
    /// Already-discovered predecessor vertex.
    NodeRef source;
    /// Newly discovered vertex.
    NodeRef target;
};

/** @brief Immutable-by-convention payload delivered to a streaming BFS callback. */
struct BfsLayerEvent {
    /// Zero-based distance of every vertex in this event.
    std::uint32_t depth = 0;
    /// Vertices discovered at @ref depth.
    std::vector<NodeRef> vertices;
    /// Tree edges that first discovered the vertices in this layer.
    std::vector<BfsTreeEdgeRef> tree_edges;
};

} // namespace cpgdb

#endif // CPGDB_BFSLAYER_HPP
