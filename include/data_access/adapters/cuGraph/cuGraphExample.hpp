//
// Created by akos on 7. 3. 2026.
//

#ifndef CPGDB_CUGRAPHEXAMPLE_HPP
#define CPGDB_CUGRAPHEXAMPLE_HPP

#include <raft/core/handle.hpp>

#include <cugraph/graph.hpp>
#include <cugraph/graph_functions.hpp>
#include <cugraph/algorithms.hpp>

#include <rmm/device_uvector.hpp>

#include <cuda_runtime.h>

#include <vector>
#include <iostream>
#include <optional>



static void cuda_check(cudaError_t err, const char* where)
{
    if (err != cudaSuccess) {
        std::cerr << "CUDA error at " << where << ": "
                  << cudaGetErrorString(err) << std::endl;
        std::exit(1);
    }
}

inline void execute()
{
    using vertex_t = int32_t;
    using edge_t   = int32_t;
    using weight_t = float;
    using result_t = float;

    raft::handle_t handle;
    auto stream = handle.get_stream();

    // ----------------------------
    // CPU graph
    // ----------------------------

    std::vector<vertex_t> h_src = {0,0,1,2,2,3};
    std::vector<vertex_t> h_dst = {1,2,2,0,3,3};
    std::vector<vertex_t> h_vertices = {0,1,2,3};

    size_t num_edges = h_src.size();

    // ----------------------------
    // Copy to GPU
    // ----------------------------

    rmm::device_uvector<vertex_t> d_src(num_edges, stream);
    rmm::device_uvector<vertex_t> d_dst(num_edges, stream);
    rmm::device_uvector<vertex_t> d_vertices(h_vertices.size(), stream);

    cuda_check(cudaMemcpyAsync(d_src.data(), h_src.data(),
                               num_edges*sizeof(vertex_t),
                               cudaMemcpyHostToDevice,
                               stream), "copy src");

    cuda_check(cudaMemcpyAsync(d_dst.data(), h_dst.data(),
                               num_edges*sizeof(vertex_t),
                               cudaMemcpyHostToDevice,
                               stream), "copy dst");

    cuda_check(cudaMemcpyAsync(d_vertices.data(), h_vertices.data(),
                               h_vertices.size()*sizeof(vertex_t),
                               cudaMemcpyHostToDevice,
                               stream), "copy vertices");

    cuda_check(cudaStreamSynchronize(stream), "sync H2D");

    // ----------------------------
    // Graph properties
    // ----------------------------

    cugraph::graph_properties_t graph_props{false,false};

    std::vector<cugraph::arithmetic_device_uvector_t> edge_props;

    // ----------------------------
    // Create graph
    // ----------------------------

    auto [graph, edge_properties, renumber_map] =
        cugraph::create_graph_from_edgelist<
            vertex_t,
            edge_t,
            true,   // store_transposed (needed for PageRank)
            false   // single GPU
        >(
            handle,
            std::optional<rmm::device_uvector<vertex_t>>(std::move(d_vertices)),
            std::move(d_src),
            std::move(d_dst),
            std::move(edge_props),
            graph_props,
            false,
            std::nullopt,
            std::nullopt,
            true
        );

    auto graph_view = graph.view();

    // ----------------------------
    // Run PageRank
    // ----------------------------

    auto [pageranks, metadata] =
        cugraph::pagerank<
            vertex_t,
            edge_t,
            weight_t,
            result_t,
            false
        >(
            handle,
            graph_view,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            result_t{0.85},
            result_t{1e-6},
            100,
            true
        );

    // ----------------------------
    // Copy results back
    // ----------------------------

    std::vector<result_t> h_pr(h_vertices.size());

    cuda_check(cudaMemcpyAsync(
        h_pr.data(),
        pageranks.data(),
        h_pr.size()*sizeof(result_t),
        cudaMemcpyDeviceToHost,
        stream), "copy pagerank");

    cuda_check(cudaStreamSynchronize(stream), "sync D2H");

    // ----------------------------
    // Print
    // ----------------------------

    for(size_t i=0;i<h_pr.size();i++)
    {
        std::cout << "vertex "
                  << i
                  << " pagerank = "
                  << h_pr[i]
                  << std::endl;
    }

}

#endif //CPGDB_CUGRAPHEXAMPLE_HPP