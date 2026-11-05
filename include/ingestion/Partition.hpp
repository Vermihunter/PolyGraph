#ifndef POLYGRAPH_INGESTION_PARTITION_HPP
#define POLYGRAPH_INGESTION_PARTITION_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace polygraph::ingestion {
    // Dense indices are temporary topology coordinates, never GlobalIds.
    // Rows must be sorted, unique, symmetric and free of self loops.
    struct GraphTopology {
        std::vector<std::uint64_t> offsets;
        std::vector<std::uint32_t> neighbors;
    };
    struct PartitionOptions {
        unsigned count = 0;
        unsigned threads = 1;
        double imbalance = 0.03;
        std::string preset = "default";
    };
    struct PartitionResult {
        std::vector<std::uint32_t> assignment;
        std::vector<std::uint64_t> populations;
        std::uint64_t edge_cut = 0;
        std::uint64_t maximum_block_size = 0;
        double library_seconds = 0;
        bool balanced = false;
    };

    void validate_topology(const GraphTopology &graph);
    void validate_partition_options(const PartitionOptions &options, unsigned seed);
    [[nodiscard]] std::uint64_t topology_edge_cut(const GraphTopology &graph,
                                                  std::span<const std::uint32_t> assignment);
    [[nodiscard]] bool kaminpar_available() noexcept;
    [[nodiscard]] std::string kaminpar_identity();
    [[nodiscard]] PartitionResult partition_topology(const GraphTopology &graph, const PartitionOptions &options,
                                                     unsigned seed);
} // namespace polygraph::ingestion
#endif
