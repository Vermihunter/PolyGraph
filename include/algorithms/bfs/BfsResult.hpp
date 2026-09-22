/**
 * @file include/algorithms/bfs/BfsResult.hpp
 * @brief External-ID result representation returned by BFS executors.
 */

#ifndef POLYGRAPH_BFSRESULT_HPP
#define POLYGRAPH_BFSRESULT_HPP

#include "data_model/EntityId.hpp"

#include <cstdint>
#include <unordered_map>

namespace polygraph {

/** @brief Distances and optional predecessor tree keyed by stable graph ID. */
struct BfsResult {
    /// Minimum number of traversed edges from the source to each reachable vertex.
    std::unordered_map<GlobalId, std::uint32_t, std::hash<GlobalId>> distance;
    /// First predecessor selected for each discovered non-source vertex.
    std::unordered_map<GlobalId, GlobalId, std::hash<GlobalId>> predecessor;
};

} // namespace polygraph

#endif // POLYGRAPH_BFSRESULT_HPP
