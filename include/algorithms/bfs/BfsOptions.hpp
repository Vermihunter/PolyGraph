/**
 * @file include/algorithms/bfs/BfsOptions.hpp
 * @brief Configures native, distributed, and GPU breadth-first search execution.
 */

#ifndef CPGDB_BFSOPTIONS_HPP
#define CPGDB_BFSOPTIONS_HPP

#include <cinttypes>
#include <cstddef>
#include <optional>

namespace cpgdb {

/** @brief Runtime options shared by every BFS implementation. */
struct BfsOptions {
    /// Maximum number of edges from the source; no value means unlimited depth.
    std::optional<std::uint32_t> max_depth;
    /// Whether the result must contain one predecessor for each discovered vertex.
    bool return_predecessors = true;
    /// Allow a capable backend to execute the complete BFS natively.
    bool allow_native_pushdown = true;
    /// Allow caches and projections to participate in distributed layer expansion.
    bool allow_partial_acceleration = true;
    /// Maximum number of frontier vertices included in one backend neighbor request.
    std::size_t neighbor_batch_size = 10'000;
    /// Requested worker count for the reusable BFS thread pool (not used by batching yet).
    std::size_t thread_count = 1;
};

} // namespace cpgdb

#endif // CPGDB_BFSOPTIONS_HPP
