/**
 * @file include/gpu/algorithms/CuGraphBfsExecutor.hpp
 * @brief Declarations for cu graph bfs executor in the GPU acceleration module.
 */

#ifndef POLYGRAPH_CUGRAPHBFSEXECUTOR_HPP
#define POLYGRAPH_CUGRAPHBFSEXECUTOR_HPP

#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "gpu/data_model/CsrGraphProjection.hpp"
#include "events/Events.hpp"

#include <cstdint>
#include <cugraph/algorithms.hpp>
#include <limits>
#include <raft/util/cudart_utils.hpp>
#include <rmm/device_uvector.hpp>
#include <stdexcept>
#include <vector>

namespace polygraph {
class CuGraphBfsExecutor {
  public:
    template <typename vertex_t, typename edge_t, bool MULTI_GPU = false>
    static BfsResult run(CsrGraphProjection<vertex_t, edge_t>& projection,
                         const GlobalId& source_id,
                         const BfsOptions& options) {
        EVENT_SCOPE_INFO(total_span, "cugraph.bfs", {"projection_vertices", projection.vertex_count()},
                         {"return_predecessors", options.return_predecessors});
        if (!projection.mapper().contains(source_id)) {
            throw std::runtime_error("Source vertex is not present in this GPU projection");
        }

        vertex_t source_dense{};
        {
            EVENT_SCOPE_INFO(map_span, "cugraph.bfs.source_to_gpu_id");
            source_dense = projection.mapper().at(source_id);
            EVENT_SCOPE_INFO_FIELD(map_span, "dense_source", source_dense);
        }

        auto& handle = projection.handle();
        auto stream = handle.get_stream();
        const std::size_t n = projection.vertex_count();

        if (n == 0) {
            throw std::runtime_error("GPU projection has zero vertices");
        }

        auto d_distances = [&]() {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.bfs.device_allocate.distances",
                             {"vertices", n}, {"bytes", n * sizeof(vertex_t)});
            return rmm::device_uvector<vertex_t>(n, stream);
        }();
        auto d_predecessors = [&]() {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.bfs.device_allocate.predecessors",
                             {"vertices", n}, {"bytes", n * sizeof(vertex_t)},
                             {"used", options.return_predecessors});
            return rmm::device_uvector<vertex_t>(n, stream);
        }();
        auto d_sources = [&]() {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.bfs.device_allocate.sources",
                             {"vertices", 1}, {"bytes", sizeof(vertex_t)});
            return rmm::device_uvector<vertex_t>(1, stream);
        }();

        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.bfs.source_h2d_enqueue", {"bytes", sizeof(vertex_t)});
            raft::update_device(d_sources.data(), &source_dense, 1, stream);
        }

        const vertex_t depth_limit = options.max_depth.has_value()
                                         ? static_cast<vertex_t>(*options.max_depth)
                                         : std::numeric_limits<vertex_t>::max();
        vertex_t* predecessors_ptr = options.return_predecessors ? d_predecessors.data() : nullptr;

        {
            // This is the host-side cuGraph API duration. If cuGraph enqueues work
            // asynchronously, the remaining GPU time will appear in stream_wait.
            EVENT_SCOPE_INFO(kernel_span, "cugraph.bfs.kernel_api", {"depth_limit", depth_limit});
            cugraph::bfs<vertex_t, edge_t, MULTI_GPU>(handle,
                                                      projection.view(),
                                                      d_distances.data(),
                                                      predecessors_ptr,
                                                      d_sources.data(),
                                                      1,
                                                      false,
                                                      depth_limit,
                                                      false);
        }

        std::vector<vertex_t> h_distances;
        std::vector<vertex_t> h_predecessors;
        {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.bfs.host_result_allocate",
                             {"vertices", n},
                             {"bytes", n * sizeof(vertex_t) * (options.return_predecessors ? 2U : 1U)});
            h_distances.resize(n);
            if (options.return_predecessors) {
                h_predecessors.resize(n);
            }
        }

        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.bfs.results_d2h_enqueue",
                             {"distance_bytes", n * sizeof(vertex_t)},
                             {"predecessor_bytes", options.return_predecessors ? n * sizeof(vertex_t) : 0U});
            raft::update_host(h_distances.data(), d_distances.data(), n, stream);
            if (options.return_predecessors) {
                raft::update_host(h_predecessors.data(), d_predecessors.data(), n, stream);
            }
        }

        {
            EVENT_SCOPE_INFO(wait_span, "cugraph.bfs.stream_wait", {"vertices", n});
            handle.sync_stream();
        }

        auto result = to_external_result(projection, h_distances, h_predecessors, options.return_predecessors);
        EVENT_SCOPE_INFO_FIELD(total_span, "result_vertices", result.distance.size());
        EVENT_SCOPE_INFO_FIELD(total_span, "result_predecessors", result.predecessor.size());
        return result;
    }

  private:
    template <typename vertex_t, typename edge_t>
    static BfsResult to_external_result(const CsrGraphProjection<vertex_t, edge_t>& projection,
                                        const std::vector<vertex_t>& h_distances,
                                        const std::vector<vertex_t>& h_predecessors,
                                        bool has_predecessors) {
        EVENT_SCOPE_INFO(conversion_span, "cugraph.bfs_to_intermediate_conversion",
                         {"dense_vertices", h_distances.size()}, {"has_predecessors", has_predecessors});
        BfsResult result;
        std::size_t unreachable_count = 0;

        for (std::size_t dense = 0; dense < h_distances.size(); ++dense) {
            const vertex_t distance = h_distances[dense];

            if (is_unreachable(distance)) {
                ++unreachable_count;
                continue;
            }

            const GlobalId& external_id = projection.mapper().global_id(static_cast<vertex_t>(dense));
            result.distance.emplace(external_id, static_cast<std::uint32_t>(distance));

            if (has_predecessors) {
                const vertex_t predecessor = h_predecessors[dense];

                if (!is_unreachable(predecessor)) {
                    const GlobalId& external_predecessor = projection.mapper().global_id(predecessor);
                    result.predecessor.emplace(external_id, external_predecessor);
                }
            }
        }

        EVENT_SCOPE_INFO_FIELD(conversion_span, "reachable_vertices", result.distance.size());
        EVENT_SCOPE_INFO_FIELD(conversion_span, "unreachable_vertices", unreachable_count);
        EVENT_SCOPE_INFO_FIELD(conversion_span, "predecessors", result.predecessor.size());
        return result;
    }

    template <typename vertex_t> static bool is_unreachable(vertex_t value) {
        if constexpr (std::numeric_limits<vertex_t>::is_signed) {
            return value < 0;
        } else {
            return value == std::numeric_limits<vertex_t>::max();
        }
    }
};

} // namespace polygraph

#endif // POLYGRAPH_CUGRAPHBFSEXECUTOR_HPP
