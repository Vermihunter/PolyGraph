//
// Created by akos on 8. 3. 2026.
//

#ifndef CPGDB_GPU_GRAPHEDGELIST_HPP
#define CPGDB_GPU_GRAPHEDGELIST_HPP

#include <raft/core/handle.hpp>
#include <rmm/device_uvector.hpp>

#include <cugraph/graph.hpp>
#include <cugraph/graph_functions.hpp>
#include <cugraph/algorithms.hpp>

template <
    typename vertex_t,
    typename edge_t,
    typename weight_t,
    typename result_t,
    bool STORE_TRANSPOSED = false,
    bool MULTI_GPU = false
>
struct GPU_Graph;

template <typename vertex_t ,typename edge_t, typename weight_t, typename result_t, bool STORE_TRANSPOSED, bool MULTI_GPU>
using gpu_graph_t = std::unique_ptr<GPU_Graph<vertex_t, edge_t, weight_t, result_t, STORE_TRANSPOSED, MULTI_GPU>>;


template <
    typename vertex_t,
    typename edge_t,
    typename weight_t,
    typename result_t,
    bool STORE_TRANSPOSED,
    bool MULTI_GPU
>
struct GPU_Graph {
    raft::handle_t handle_;

    cugraph::graph_t<vertex_t, edge_t, STORE_TRANSPOSED, MULTI_GPU> graph_;
    cugraph::graph_view_t<vertex_t, edge_t, STORE_TRANSPOSED, MULTI_GPU> graph_view_;
    std::vector<std::string> id_to_string_;

    GPU_Graph(raft::handle_t handle, cugraph::graph_t<vertex_t, edge_t, STORE_TRANSPOSED, MULTI_GPU> graph)
        : handle_(handle), graph_(std::move(graph)), graph_view_(graph_.view()) {}

    static gpu_graph_t<vertex_t, edge_t, weight_t, result_t, STORE_TRANSPOSED, MULTI_GPU>
    from_edge_list(raft::handle_t& handle,
                    rmm::device_uvector<vertex_t> d_src,      // Passed by value
                    rmm::device_uvector<vertex_t> d_dst,      // Passed by value
                    rmm::device_uvector<vertex_t> d_vertices, // Passed by value
                    cugraph::graph_properties_t graph_props,
                    std::vector<cugraph::arithmetic_device_uvector_t> edge_props)
    {


        auto [graph, edge_properties, renumber_map] =
            cugraph::create_graph_from_edgelist<
                vertex_t,
                edge_t,
                STORE_TRANSPOSED,
                MULTI_GPU
            >(
                handle,
                std::make_optional<rmm::device_uvector<vertex_t>>(std::move(d_vertices)),
                std::move(d_src),
                std::move(d_dst),
                std::move(edge_props),
                graph_props,
                false,        // multi_gpu
                std::nullopt, // edge_weights
                std::nullopt, // edge_id_list
                true          // do_renumber
            );

        auto gpu_graph = std::make_unique<GPU_Graph<vertex_t, edge_t, weight_t, result_t, STORE_TRANSPOSED, MULTI_GPU>>(handle, std::move(graph));
        //gpu_graph->graph_ = ;
        //gpu_graph->graph_view_ = gpu_graph->graph_.view(); // View the internal graph

        return gpu_graph;

        return gpu_graph;
    }
};

#endif //CPGDB_GPU_GRAPHEDGELIST_HPP