//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_BFSOPTIONS_HPP
#define CPGDB_BFSOPTIONS_HPP

#include <cinttypes>
#include <optional>

struct BfsOptions {
    std::optional<std::uint32_t> max_depth;
    bool return_predecessors = true;

    // If true, planner may choose Memgraph / GPU / Neo4j GDS native BFS
    // when the backend covers the whole requested subgraph.
    bool allow_native_pushdown = true;

    // If true, distributed fallback may use caches/projections for frontier expansion.
    bool allow_partial_acceleration = true;
};

#endif //CPGDB_BFSOPTIONS_HPP