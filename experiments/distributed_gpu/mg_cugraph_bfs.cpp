// Experimental native cuGraph multi-GPU BFS scaling harness.
// Intentionally kept outside the main PolyGraph build: this execution model
// requires one MPI rank per GPU and collective call ordering across all ranks.

#include <cuda_runtime.h>
#include <mpi.h>

#include <cugraph/algorithms.hpp>
#include <cugraph/graph.hpp>
#include <cugraph/graph_functions.hpp>
#include <cugraph/shuffle_functions.hpp>
#include <raft/comms/mpi_comms.hpp>
#include <raft/core/handle.hpp>
#include <raft/util/cudart_utils.hpp>
#include <rmm/device_uvector.hpp>
#include <rmm/mr/device/cuda_memory_resource.hpp>
#include <rmm/mr/device/per_device_resource.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

using vertex_t = std::int32_t;
using edge_t = std::int32_t;
constexpr bool store_transposed = false;
constexpr bool multi_gpu = true;

std::unique_ptr<raft::handle_t> make_handle() {
    auto resource = rmm::mr::cuda_memory_resource();
    rmm::mr::set_current_device_resource(resource);
    auto handle = std::make_unique<raft::handle_t>(rmm::cuda_stream_per_thread, nullptr, resource);
    raft::comms::initialize_mpi_comms(handle.get(), MPI_COMM_WORLD);

    const int world = handle->get_comms().get_size();
    int row = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(world))));
    while (world % row != 0) --row;
    cugraph::partition_manager::init_subcomm(*handle, row);
    return handle;
}

// Every rank generates a disjoint source-vertex range, then cuGraph performs
// the ownership shuffle required by its 2-D partitioning scheme.
void make_local_synthetic_edges(int rank,
                                int world,
                                vertex_t vertices,
                                int degree,
                                std::vector<vertex_t>& src,
                                std::vector<vertex_t>& dst) {
    const vertex_t first = static_cast<vertex_t>((static_cast<std::int64_t>(vertices) * rank) / world);
    const vertex_t last = static_cast<vertex_t>((static_cast<std::int64_t>(vertices) * (rank + 1)) / world);
    src.reserve(static_cast<std::size_t>(last - first) * degree);
    dst.reserve(static_cast<std::size_t>(last - first) * degree);
    for (vertex_t u = first; u < last; ++u) {
        for (int k = 1; k <= degree; ++k) {
            const auto step = static_cast<vertex_t>(1 + ((k * 2654435761ULL) % std::max<vertex_t>(1, vertices - 1)));
            src.push_back(u);
            dst.push_back(static_cast<vertex_t>((static_cast<std::int64_t>(u) + step) % vertices));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank = 0;
    int world = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world);

    int local_rank = 0;
    MPI_Comm local_comm{};
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank, MPI_INFO_NULL, &local_comm);
    MPI_Comm_rank(local_comm, &local_rank);

    int gpu_count = 0;
    RAFT_CUDA_TRY(cudaGetDeviceCount(&gpu_count));
    if (gpu_count <= 0) {
        if (rank == 0) std::cerr << "No CUDA devices found\n";
        MPI_Abort(MPI_COMM_WORLD, 2);
    }
    RAFT_CUDA_TRY(cudaSetDevice(local_rank % gpu_count));

    const vertex_t vertices = argc > 1 ? static_cast<vertex_t>(std::stoll(argv[1])) : 10'000'000;
    const int degree = argc > 2 ? std::stoi(argv[2]) : 8;
    const int iterations = argc > 3 ? std::stoi(argv[3]) : 10;
    if (vertices < 2 || degree < 1 || iterations < 1) {
        if (rank == 0) std::cerr << "Usage: mg_cugraph_bfs [vertices>=2] [degree>=1] [iterations>=1]\n";
        MPI_Abort(MPI_COMM_WORLD, 3);
    }

    auto handle = make_handle();
    auto stream = handle->get_stream();

    std::vector<vertex_t> h_src;
    std::vector<vertex_t> h_dst;
    make_local_synthetic_edges(rank, world, vertices, degree, h_src, h_dst);

    rmm::device_uvector<vertex_t> d_src(h_src.size(), stream);
    rmm::device_uvector<vertex_t> d_dst(h_dst.size(), stream);
    raft::update_device(d_src.data(), h_src.data(), h_src.size(), stream);
    raft::update_device(d_dst.data(), h_dst.data(), h_dst.size(), stream);

    std::vector<cugraph::arithmetic_device_uvector_t> edge_properties;
    std::tie(d_src, d_dst, edge_properties, std::ignore) =
        cugraph::shuffle_ext_edges(*handle, std::move(d_src), std::move(d_dst),
                                   std::move(edge_properties), store_transposed);

    cugraph::graph_t<vertex_t, edge_t, store_transposed, multi_gpu> graph(*handle);
    std::vector<cugraph::edge_arithmetic_property_t<edge_t>> created_edge_properties;
    std::optional<rmm::device_uvector<vertex_t>> renumber_map;
    std::tie(graph, created_edge_properties, renumber_map) =
        cugraph::create_graph_from_edgelist<vertex_t, edge_t, store_transposed, multi_gpu>(
            *handle,
            std::nullopt,
            std::move(d_src),
            std::move(d_dst),
            std::move(edge_properties),
            cugraph::graph_properties_t{false, false},
            true,   // native MG requires renumbering
            std::nullopt,
            std::nullopt,
            false);

    auto graph_view = graph.view();
    rmm::device_uvector<vertex_t> d_distances(graph_view.local_vertex_partition_range_size(), stream);
    rmm::device_uvector<vertex_t> d_predecessors(graph_view.local_vertex_partition_range_size(), stream);
    rmm::device_uvector<vertex_t> d_sources(1, stream);

    // This harness intentionally uses internal source 0, matching the minimal
    // libcugraph MG example style. A PolyGraph integration must translate the
    // external GlobalId through the distributed renumber map first.
    vertex_t source = 0;
    raft::update_device(d_sources.data(), &source, 1, stream);
    handle->sync_stream();

    // Warm-up collective.
    cugraph::bfs(*handle, graph_view, d_distances.data(), d_predecessors.data(),
                 d_sources.data(), 1, false, std::numeric_limits<vertex_t>::max());
    handle->sync_stream();
    MPI_Barrier(MPI_COMM_WORLD);

    double best_ms = std::numeric_limits<double>::max();
    double sum_ms = 0.0;
    for (int i = 0; i < iterations; ++i) {
        MPI_Barrier(MPI_COMM_WORLD);
        const double t0 = MPI_Wtime();
        cugraph::bfs(*handle, graph_view, d_distances.data(), d_predecessors.data(),
                     d_sources.data(), 1, false, std::numeric_limits<vertex_t>::max());
        handle->sync_stream();
        const double local_ms = (MPI_Wtime() - t0) * 1000.0;
        double global_ms = 0.0;
        MPI_Allreduce(&local_ms, &global_ms, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
        best_ms = std::min(best_ms, global_ms);
        sum_ms += global_ms;
    }

    if (rank == 0) {
        std::cout << "native-mg-cugraph-bfs"
                  << " ranks=" << world
                  << " vertices=" << vertices
                  << " degree=" << degree
                  << " iterations=" << iterations
                  << " best_ms=" << best_ms
                  << " mean_ms=" << (sum_ms / iterations)
                  << '\n';
    }

    handle.reset();
    MPI_Comm_free(&local_comm);
    MPI_Finalize();
    return 0;
}
