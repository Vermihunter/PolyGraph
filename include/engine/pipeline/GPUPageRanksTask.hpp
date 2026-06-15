//
// Created by akos on 8. 3. 2026.
//

#ifndef CPGDB_GPUPAGERANKSTASK_HPP
#define CPGDB_GPUPAGERANKSTASK_HPP

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
auto gpu_pageranks(gpu_graph_t<vertex_t, edge_t, weight_t, result_t, store_transposed, multi_gpu> gpudata) {
    auto start = std::chrono::high_resolution_clock::now();
    auto [pageranks, metadata] =
            cugraph::pagerank<
                vertex_t, edge_t, weight_t, result_t, multi_gpu
            >(
                gpudata->handle_,
                gpudata->graph_view_,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                result_t{0.85},
                result_t{1e-6},
                100,
                true
            );


    auto end = std::chrono::high_resolution_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "The PageRank algorithm took " << time.count() << "ms time" << std::endl;


    return std::pair{std::move(gpudata), std::move(pageranks)};
}

#endif //CPGDB_GPUPAGERANKSTASK_HPP