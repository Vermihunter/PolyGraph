/**
 * @file include/algorithms/bfs/BfsOptions.hpp
 * @brief Configures native, distributed, and GPU breadth-first search execution.
 */

#ifndef POLYGRAPH_BFSOPTIONS_HPP
#define POLYGRAPH_BFSOPTIONS_HPP

#include <cinttypes>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace polygraph {

/** @brief Controls how ArangoDB frontier vertices are packed into HTTP/AQL requests. */
enum class ArangoRequestStrategy {
    /// Split each source collection frontier into neighbor_batch_size chunks.
    Chunked,
    /// Send the complete source-collection frontier in one request per edge-index scan.
    Single,
    /// Use Single for small frontiers and Chunked once arango_single_request_max_frontier is exceeded.
    Auto,
};

[[nodiscard]] constexpr std::string_view to_string(const ArangoRequestStrategy strategy) noexcept {
    switch (strategy) {
        case ArangoRequestStrategy::Chunked:
            return "chunked";
        case ArangoRequestStrategy::Single:
            return "single";
        case ArangoRequestStrategy::Auto:
            return "auto";
    }
    return "chunked";
}

[[nodiscard]] inline ArangoRequestStrategy parse_arango_request_strategy(const std::string_view value) {
    if (value == "chunked") {
        return ArangoRequestStrategy::Chunked;
    }
    if (value == "single") {
        return ArangoRequestStrategy::Single;
    }
    if (value == "auto") {
        return ArangoRequestStrategy::Auto;
    }
    throw std::invalid_argument("Arango request strategy must be chunked, single, or auto");
}

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
    /// Maximum number of frontier vertices included in one chunked backend neighbor request.
    std::size_t neighbor_batch_size = 10'000;
    /// Per-BFS Neo4j concurrency cap; the server-global backend scheduler may impose a lower limit.
    std::size_t neo4j_max_concurrent_requests = 4;
    /// Per-BFS ArangoDB concurrency cap; the server-global backend scheduler may impose a lower limit.
    std::size_t arango_max_concurrent_requests = 4;
    /// How ArangoDB frontier vertices are packed into requests.
    ArangoRequestStrategy arango_request_strategy = ArangoRequestStrategy::Chunked;
    /// In Auto mode, source-collection frontiers at or below this size use a single request per edge scan.
    std::size_t arango_single_request_max_frontier = 50'000;
    /// Maximum number of candidate vertices emitted to BFS in one transient callback batch.
    std::size_t streaming_candidate_batch_size = 16'384;
    /// Maximum Redis keys fetched in one pipeline while streaming BFS candidates.
    std::size_t redis_pipeline_batch_size = 20'000;
    /// Stable ID used by server-global backend schedulers to group one BFS request. Zero means auto-assign.
    std::uint64_t scheduler_query_id = 0;
    /// Higher values receive preference under priority-aware backend scheduling policies.
    int backend_scheduler_priority = 0;
    /// Relative share under weighted-fair backend scheduling policies.
    std::size_t backend_scheduler_weight = 1;
    /// Number of retries after the initial backend request attempt.
    std::size_t backend_request_retries = 2;
    /// Initial retry delay; each subsequent retry doubles this value.
    std::size_t backend_retry_delay_ms = 100;
    /// Requested worker count for the reusable BFS thread pool.
    std::size_t thread_count = 1;
};

} // namespace polygraph

#endif // POLYGRAPH_BFSOPTIONS_HPP
