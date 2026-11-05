#include "ingestion/Partition.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include "ingestion/Import.hpp"

namespace polygraph::ingestion {
    void validate_topology(const GraphTopology &graph) {
        if (graph.offsets.size() < 2 || graph.offsets.size() - 1 >= std::numeric_limits<std::uint32_t>::max())
            throw Error("Partition topology requires 1..2^32-2 vertices");
        if (graph.offsets.front() != 0 || graph.offsets.back() != graph.neighbors.size())
            throw Error("Invalid partition CSR offsets/adjacency size");
        const auto n = graph.offsets.size() - 1;
        for (std::size_t u = 0; u < n; ++u) {
            const auto begin = graph.offsets[u], end = graph.offsets[u + 1];
            if (begin > end || end > graph.neighbors.size())
                throw Error("Partition CSR offsets must be monotone and in range");
            const auto row = std::span(graph.neighbors).subspan(begin, end - begin);
            if (!std::ranges::is_sorted(row) || std::adjacent_find(row.begin(), row.end()) != row.end())
                throw Error("Partition CSR rows must be sorted and deduplicated");
            for (const auto v: row)
                if (v >= n || v == u)
                    throw Error("Partition CSR neighbor is out of range or a self loop");
        }
        for (std::size_t u = 0; u < n; ++u)
            for (auto edge = graph.offsets[u]; edge < graph.offsets[u + 1]; ++edge) {
                const auto v = graph.neighbors[edge];
                const auto reverse =
                        std::span(graph.neighbors).subspan(graph.offsets[v], graph.offsets[v + 1] - graph.offsets[v]);
                if (!std::binary_search(reverse.begin(), reverse.end(), u))
                    throw Error("Partition CSR must contain both directions of every undirected edge");
            }
    }

    void validate_partition_options(const PartitionOptions &options, unsigned seed) {
        if (options.count < 2 || options.count == std::numeric_limits<std::uint32_t>::max())
            throw Error("KaMinPar requires --partitions in [2,2^32-2]");
        if (options.threads < 1 || options.threads > 256)
            throw Error("--partition-threads must be in [1,256]");
        if (!std::isfinite(options.imbalance) || options.imbalance < 0 || options.imbalance > 1)
            throw Error("--partition-imbalance must be finite and in [0,1]");
        if (options.preset != "default" && options.preset != "eco" && options.preset != "strong")
            throw Error("--partition-preset must be default, eco or strong");
        if (seed > static_cast<unsigned>(std::numeric_limits<int>::max()))
            throw Error("KaMinPar --seed must fit a nonnegative signed int");
    }

    std::uint64_t topology_edge_cut(const GraphTopology &graph, std::span<const std::uint32_t> assignment) {
        validate_topology(graph);
        if (assignment.size() + 1 != graph.offsets.size())
            throw Error("Partition assignment must contain one entry per topology vertex");
        std::uint64_t cut = 0;
        for (std::size_t u = 0; u < assignment.size(); ++u)
            for (auto edge = graph.offsets[u]; edge < graph.offsets[u + 1]; ++edge) {
                const auto v = graph.neighbors[edge];
                if (v > u && assignment[u] != assignment[v])
                    ++cut;
            }
        return cut;
    }
} // namespace polygraph::ingestion
