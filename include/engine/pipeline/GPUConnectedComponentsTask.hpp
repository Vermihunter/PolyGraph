//
// Created by akos on 8. 3. 2026.
//

#ifndef CPGDB_CONNECTEDCOMPONENTSTASK_HPP
#define CPGDB_CONNECTEDCOMPONENTSTASK_HPP

#include <cugraph/algorithms.hpp>

#include "gpu/GPU_Graph.hpp"


template <
    typename vertex_t,
    typename edge_t,
    typename weight_t,
    typename result_t,
    bool store_transposed,
    bool multi_gpu
>
auto gpu_triangle_count(gpu_graph_t<vertex_t, edge_t, weight_t, result_t,store_transposed, multi_gpu> gpudata) {
    auto num_vertices = gpudata->graph_view_.number_of_vertices();
    auto stream = gpudata->handle_.get_stream();

    // 1. Allocate the output vector
    rmm::device_uvector<edge_t> triangle_counts(num_vertices, stream);

    // 2. Wrap the output in a raft::device_span
    // A span is just a view (pointer + size) that doesn't own the memory
    raft::device_span<edge_t> counts_span(triangle_counts.data(), triangle_counts.size());


    std::cout << "gpu connected components running - num_vertices=" << num_vertices << std::endl;

    // 3. Call the algorithm
    // We pass std::nullopt for vertices to calculate for the WHOLE graph
    try {
        cugraph::triangle_count<vertex_t, edge_t, multi_gpu>(
            gpudata->handle_,
            gpudata->graph_view_,
            std::nullopt,
            counts_span,
            false
        );

        // FORCE CHECK HERE
        cuda_check(cudaStreamSynchronize(stream), "Triangle count kernel failed");
    } catch (const std::exception& e) {
        std::cerr << "Caught exception: " << e.what() << std::endl;
    }

    std::cout << "gpu connected components finished!" << std::endl;

    /*
    cugraph::weakly_connected_components<vertex_t, edge_t, false>(
        gpudata->handle_,
        gpudata->graph_view_,
        component_id.data(),
        false);
    */

    std::cout << "gpu connected components running - num_vertices=" << num_vertices << std::endl;
    return std::pair{std::move(gpudata), std::move(triangle_counts)};

}

#include <cugraph/edge_property.hpp>


template <
    typename vertex_t,
    typename edge_t,
    typename weight_t,
    typename result_t,
    bool store_transposed,
    bool multi_gpu
>
auto gpu_bfs(gpu_graph_t<vertex_t, edge_t, weight_t, result_t, store_transposed, multi_gpu> gpudata) {
    vertex_t source_vertex = 0;
    auto num_vertices = gpudata->graph_view_.number_of_vertices();
    auto stream = gpudata->handle_.get_stream();

    rmm::device_uvector<vertex_t> distances(num_vertices, stream);

    // Expects a span of source vertices
    std::vector<vertex_t> h_sources = {source_vertex};
    rmm::device_uvector<vertex_t> d_sources(h_sources.size(), stream);
    raft::update_device(d_sources.data(), h_sources.data(), h_sources.size(), stream);

    std::cout << "Running BFS from source: " << source_vertex << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    // BFS call
    cugraph::bfs<vertex_t, edge_t, multi_gpu>(
        gpudata->handle_,
        gpudata->graph_view_,
        distances.data(),                     // distances
        nullptr,                              // predecessors
        d_sources.data(),                     // pointer to sources on device
        static_cast<size_t>(d_sources.size()), // number of sources
        false,                                // direction_optimizing
        std::numeric_limits<vertex_t>::max(), // depth_limit
        false                                 // do_expensive_check
    );

    cuda_check(cudaStreamSynchronize(stream), "BFS Kernel Failed");

    auto end = std::chrono::high_resolution_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "BFS took " << time.count() << "ms" << std::endl;

    return std::pair{std::move(gpudata), std::move(distances)};
}

#endif //CPGDB_CONNECTEDCOMPONENTSTASK_HPP