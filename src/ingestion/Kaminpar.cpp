#include "ingestion/Partition.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include "ingestion/Import.hpp"

#if POLYGRAPH_INGESTION_HAS_KAMINPAR
#include <dlfcn.h>
#include <kaminpar.h>
#include <oneapi/tbb/version.h>
#endif

namespace polygraph::ingestion {
    bool kaminpar_available() noexcept {
#if POLYGRAPH_INGESTION_HAS_KAMINPAR
        return true;
#else
        return false;
#endif
    }

    std::string kaminpar_identity() {
#if POLYGRAPH_INGESTION_HAS_KAMINPAR
        // Hash the actual loaded modules, not only version strings/Git HEAD:
        // packages can be rebuilt in place, and source overrides can be dirty.
        // For static KaMinPar this conservatively fingerprints the executable.
        static const std::string identity = [] {
            const auto module_hash = [](const void *symbol) {
                Dl_info module{};
                if (::dladdr(symbol, &module) == 0 || module.dli_fname == nullptr)
                    throw Error("Cannot fingerprint the loaded partitioner dependency");
                return sha256_file(fs::canonical(module.dli_fname));
            };
            return "KaMinPar/" + std::to_string(KAMINPAR_VERSION_MAJOR) + "." + std::to_string(KAMINPAR_VERSION_MINOR) +
                   "." + std::to_string(KAMINPAR_VERSION_PATCH) + ";" + POLYGRAPH_KAMINPAR_IDENTITY +
                   ";module-sha256:" + module_hash(reinterpret_cast<const void *>(&kaminpar::KaMinPar::reseed)) +
                   ";tbb-sha256:" + module_hash(reinterpret_cast<const void *>(&TBB_runtime_interface_version));
        }();
        return identity;
#else
        return "unavailable";
#endif
    }

    PartitionResult partition_topology(const GraphTopology &graph, const PartitionOptions &options, unsigned seed) {
        if (!kaminpar_available())
            throw Error(
                    "KaMinPar is unavailable; build with POLYGRAPH_INGESTION_KAMINPAR=ON or use --partitioner none");
        validate_partition_options(options, seed);
        validate_topology(graph);
        if (options.count >= graph.offsets.size())
            throw Error("--partitions exceeds the topology vertex count");
#if POLYGRAPH_INGESTION_HAS_KAMINPAR
        using namespace kaminpar;
        if (graph.neighbors.size() >= std::numeric_limits<shm::EdgeID>::max() ||
            graph.neighbors.size() > static_cast<std::uint64_t>(std::numeric_limits<shm::EdgeWeight>::max()) ||
            graph.offsets.size() - 1 > static_cast<std::uint64_t>(std::numeric_limits<shm::NodeWeight>::max()))
            throw Error("Topology exceeds this KaMinPar build's edge/weight range; use a 64-bit dependency build");
        std::vector<shm::EdgeID> offsets(graph.offsets.begin(), graph.offsets.end());
        std::vector<shm::NodeID> neighbors(graph.neighbors.begin(), graph.neighbors.end());
        // KaMinPar's random seed is process-global. Its optional timers also
        // require serialization across objects. Each call owns all graph buffers.
        static std::mutex library_mutex;
        std::scoped_lock guard(library_mutex);
        KaMinPar::reseed(static_cast<int>(seed));
        KaMinPar partitioner(static_cast<int>(options.threads), shm::create_context_by_preset_name(options.preset));
        partitioner.set_output_level(OutputLevel::QUIET);
        partitioner.copy_graph(offsets, neighbors);
        partitioner.set_k(options.count);
        partitioner.set_uniform_max_block_weights(options.imbalance);
        std::vector<shm::BlockID> assignment(graph.offsets.size() - 1);
        const auto start = std::chrono::steady_clock::now();
        const auto cut = partitioner.compute_partition(assignment);
        PartitionResult result;
        result.library_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        result.assignment.assign(assignment.begin(), assignment.end());
        result.populations.resize(options.count);
        for (const auto block: result.assignment) {
            if (block >= options.count)
                throw Error("KaMinPar returned an out-of-range partition");
            ++result.populations[block];
        }
        result.edge_cut = topology_edge_cut(graph, result.assignment);
        if (cut < 0 || static_cast<std::uint64_t>(cut) != result.edge_cut)
            throw Error("KaMinPar edge cut disagrees with independent CSR verification");
        const auto maximum = partitioner.context().partition.max_block_weight(0);
        if (maximum < 0)
            throw Error("KaMinPar returned an invalid balance bound");
        result.maximum_block_size = static_cast<std::uint64_t>(maximum);
        result.balanced = std::ranges::all_of(result.populations,
                                              [&](const auto size) { return size <= result.maximum_block_size; });
        if (!result.balanced)
            throw Error("KaMinPar returned a partition exceeding its configured balance bound");
        return result;
#else
        throw Error("KaMinPar is unavailable");
#endif
    }
} // namespace polygraph::ingestion
