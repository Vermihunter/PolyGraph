//
// Created by akos on 8. 3. 2026.
//

#ifndef CPGDB_PAGERANKSRESULTCOPYBACKTASK_HPP
#define CPGDB_PAGERANKSRESULTCOPYBACKTASK_HPP

#include "GPU/GPU_Graph.hpp"

template <
    typename T,
    typename vertex_t,
    typename edge_t,
    typename weight_t,
    typename result_t,
    bool store_transposed,
    bool multi_gpu
>
auto pagerank_result_to_cpu(
    std::pair<
        gpu_graph_t<vertex_t, edge_t, weight_t, result_t, store_transposed, multi_gpu>,
        rmm::device_uvector<T>
    > &&results
) -> std::vector<T> {
    std::cout << "Pagerank result to cpu" << std::endl;
    auto&&[gpudata, pageranks] = results;

    // Number of distinct nodes in the edges
    std::vector<T> h_pr(gpudata->graph_view_.number_of_vertices());
    std::cout << "Num vertices at pagerank_result_to_cpu=" << gpudata->graph_view_.number_of_vertices() << std::endl;

    auto stream = gpudata->handle_.get_stream();
    cuda_check(cudaMemcpyAsync(
        h_pr.data(),
        pageranks.data(),
        h_pr.size()*sizeof(T),
        cudaMemcpyDeviceToHost,
        stream), "copy pagerank");

    cuda_check(cudaStreamSynchronize(stream), "sync D2H");

    return h_pr;
}

#endif //CPGDB_PAGERANKSRESULTCOPYBACKTASK_HPP