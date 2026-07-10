#ifndef CPGDB_CUGRAPHBFSEXECUTOR_HPP
#define CPGDB_CUGRAPHBFSEXECUTOR_HPP

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <cugraph/algorithms.hpp>
#include <raft/util/cudart_utils.hpp>
#include <rmm/device_uvector.hpp>

#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "gpu/data_model/CsrGraphProjection.hpp"

class CuGraphBfsExecutor {
public:
    template <typename vertex_t, typename edge_t, bool MULTI_GPU = false>
    static BfsResult    run(CsrGraphProjection<vertex_t, edge_t>& projection,
                         const GlobalId& source_id,
                         const BfsOptions& options) {
        if (!projection.mapper().contains(source_id)) {
            throw std::runtime_error(
                "Source vertex is not present in this GPU projection"
            );
        }

        vertex_t source_dense = projection.mapper().at(source_id);

        auto& handle = projection.handle();
        auto stream = handle.get_stream();

        const std::size_t n = projection.vertex_count();

        if (n == 0) {
            throw std::runtime_error("GPU projection has zero vertices");
        }

        EVENT_SCOPE(_, "cugraph.bfs");

        rmm::device_uvector<vertex_t> d_distances(n, stream);
        rmm::device_uvector<vertex_t> d_predecessors(n, stream);
        rmm::device_uvector<vertex_t> d_sources(1, stream);

        raft::update_device(
            d_sources.data(),
            &source_dense,
            1,
            stream
        );

        vertex_t depth_limit =
            options.max_depth.has_value()
                ? static_cast<vertex_t>(*options.max_depth)
                : std::numeric_limits<vertex_t>::max();

        vertex_t* predecessors_ptr =
            options.return_predecessors
                ? d_predecessors.data()
                : nullptr;

        cugraph::bfs<vertex_t, edge_t, MULTI_GPU>(
            handle,
            projection.view(),
            d_distances.data(),
            predecessors_ptr,
            d_sources.data(),
            1,
            false,
            depth_limit,
            false
        );

        std::vector<vertex_t> h_distances(n);
        std::vector<vertex_t> h_predecessors;

        raft::update_host(
            h_distances.data(),
            d_distances.data(),
            n,
            stream
        );

        if (options.return_predecessors) {
            h_predecessors.resize(n);

            raft::update_host(
                h_predecessors.data(),
                d_predecessors.data(),
                n,
                stream
            );
        }

        handle.sync_stream();

        return to_external_result(
            projection,
            h_distances,
            h_predecessors,
            options.return_predecessors
        );
    }

private:
    template <typename vertex_t, typename edge_t>
    static BfsResult to_external_result(
        const CsrGraphProjection<vertex_t, edge_t>& projection,
        const std::vector<vertex_t>& h_distances,
        const std::vector<vertex_t>& h_predecessors,
        bool has_predecessors
    ) {
        EVENT_SCOPE(_, "cugraph.bfs_to_intermediate_conversion");
        BfsResult result;

        for (std::size_t dense = 0; dense < h_distances.size(); ++dense) {
            vertex_t distance = h_distances[dense];

            if (is_unreachable(distance)) {
                continue;
            }

            const GlobalId& external_id =
                projection.mapper().global_id(static_cast<vertex_t>(dense));

            result.distance.emplace(
                external_id,
                static_cast<std::uint32_t>(distance)
            );

            if (has_predecessors) {
                vertex_t predecessor = h_predecessors[dense];

                if (!is_unreachable(predecessor)) {
                    const GlobalId& external_predecessor =
                        projection.mapper().global_id(predecessor);

                    result.predecessor.emplace(
                        external_id,
                        external_predecessor
                    );
                }
            }
        }

        return result;
    }

    template <typename vertex_t>
    static bool is_unreachable(vertex_t value) {
        if constexpr (std::numeric_limits<vertex_t>::is_signed) {
            return value < 0;
        } else {
            return value == std::numeric_limits<vertex_t>::max();
        }
    }
};

#endif // CPGDB_CUGRAPHBFSEXECUTOR_HPP
