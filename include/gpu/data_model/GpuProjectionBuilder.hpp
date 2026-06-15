#ifndef CPGDB_GPUPROJECTIONBUILDER_HPP
#define CPGDB_GPUPROJECTIONBUILDER_HPP

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>

#include <cugraph/graph.hpp>
#include <cugraph/graph_functions.hpp>
#include <raft/core/handle.hpp>
#include <rmm/device_uvector.hpp>

#include "data_model/Edge.hpp"
#include "data_model/Node.hpp"
#include "data_model/Record.hpp"
#include "gpu/data_model/CsrGraphProjection.hpp"
#include "query/QueryResult.hpp"

static inline void gpu_cuda_check(cudaError_t err, const char* where) {
    if (err != cudaSuccess) {
        throw std::runtime_error(
            std::string("CUDA error at ") + where + ": " + cudaGetErrorString(err)
        );
    }
}

class GpuProjectionBuilder {
public:
    template <typename vertex_t, typename edge_t>
    static std::shared_ptr<CsrGraphProjection<vertex_t, edge_t>>
    from_records(raft::handle_t& handle,
                 const std::vector<record_t>& records,
                 std::uint64_t graph_version,
                 bool mirror_edges_for_undirected_expansion = false) {
        auto stream = handle.get_stream();

        DenseIdMapper<vertex_t> mapper;
        std::vector<vertex_t> h_src;
        std::vector<vertex_t> h_dst;

        // ------------------------------------------------------------
        // 1. Register explicit Node records.
        //
        // Expected input shape:
        //   Record.fields[0] = Value(Node)
        // or:
        //   Record.fields[0] = Value(Edge)
        // ------------------------------------------------------------
        for (const auto& record_ptr : records) {
            if (!record_ptr) {
                continue;
            }

            const Record& record = *record_ptr;

            for (const auto& field : record.fields) {
                if (field.template is<Node>()) {
                    const Node& node = field.template as<Node>();
                    mapper.get_or_add(node.global_id);
                }
            }
        }

        // ------------------------------------------------------------
        // 2. Register Edge records and their endpoints.
        // ------------------------------------------------------------
        for (const auto& record_ptr : records) {
            if (!record_ptr) {
                continue;
            }

            const Record& record = *record_ptr;

            for (const auto& field : record.fields) {
                if (!field.template is<Edge>()) {
                    continue;
                }

                const Edge& edge = field.template as<Edge>();

                vertex_t src = mapper.get_or_add(edge.src);
                vertex_t dst = mapper.get_or_add(edge.dst);

                h_src.push_back(src);
                h_dst.push_back(dst);

                if (mirror_edges_for_undirected_expansion) {
                    h_src.push_back(dst);
                    h_dst.push_back(src);
                }
            }
        }

        // ------------------------------------------------------------
        // 3. Explicit dense vertex list: 0, 1, 2, ..., n - 1.
        // ------------------------------------------------------------
        std::vector<vertex_t> h_vertices;
        h_vertices.reserve(mapper.size());

        for (std::size_t i = 0; i < mapper.size(); ++i) {
            h_vertices.push_back(static_cast<vertex_t>(i));
        }

        const std::size_t num_vertices = h_vertices.size();
        const std::size_t num_edges = h_src.size();

        std::cout << "GPU projection vertices: " << num_vertices << '\n';
        std::cout << "GPU projection edges: " << num_edges << '\n';

        if (num_vertices == 0) {
            throw std::runtime_error("Cannot build cuGraph projection: no vertices");
        }

        for (vertex_t v : h_src) {
            if (static_cast<std::size_t>(v) >= num_vertices) {
                throw std::runtime_error("Invalid source dense vertex id");
            }
        }

        for (vertex_t v : h_dst) {
            if (static_cast<std::size_t>(v) >= num_vertices) {
                throw std::runtime_error("Invalid destination dense vertex id");
            }
        }

        // ------------------------------------------------------------
        // 4. Copy to GPU exactly in the same style as your working example.
        // ------------------------------------------------------------
        rmm::device_uvector<vertex_t> d_src(num_edges, stream);
        rmm::device_uvector<vertex_t> d_dst(num_edges, stream);
        rmm::device_uvector<vertex_t> d_vertices(num_vertices, stream);

        if (num_edges > 0) {
            gpu_cuda_check(
                cudaMemcpyAsync(
                    d_src.data(),
                    h_src.data(),
                    num_edges * sizeof(vertex_t),
                    cudaMemcpyHostToDevice,
                    stream
                ),
                "copy src"
            );

            gpu_cuda_check(
                cudaMemcpyAsync(
                    d_dst.data(),
                    h_dst.data(),
                    num_edges * sizeof(vertex_t),
                    cudaMemcpyHostToDevice,
                    stream
                ),
                "copy dst"
            );
        }

        gpu_cuda_check(
            cudaMemcpyAsync(
                d_vertices.data(),
                h_vertices.data(),
                num_vertices * sizeof(vertex_t),
                cudaMemcpyHostToDevice,
                stream
            ),
            "copy vertices"
        );

        gpu_cuda_check(cudaStreamSynchronize(stream), "sync H2D");

        // ------------------------------------------------------------
        // 5. Create cuGraph graph.
        //
        // For BFS, store_transposed must be false.
        // We already dense-renumbered manually, so cuGraph renumber=false.
        // ------------------------------------------------------------
        cugraph::graph_properties_t graph_props{
            false,  // is_symmetric: keep directed semantics; mirrored edges are explicit
            false   // is_multigraph
        };

        std::vector<cugraph::arithmetic_device_uvector_t> edge_props;

        using projection_t = CsrGraphProjection<vertex_t, edge_t>;

        auto [graph, edge_properties, renumber_map] =
            cugraph::create_graph_from_edgelist<
                vertex_t,
                edge_t,
                projection_t::store_transposed,
                projection_t::multi_gpu
            >(
                handle,
                std::optional<rmm::device_uvector<vertex_t>>(std::move(d_vertices)),
                std::move(d_src),
                std::move(d_dst),
                std::move(edge_props),
                graph_props,
                false,       // renumber=false because mapper already produced dense ids
                std::nullopt,
                std::nullopt,
                true
            );

        handle.sync_stream();

        return std::make_shared<projection_t>(
            handle,
            std::move(graph),
            std::move(mapper),
            graph_version
        );
    }
};

#endif // CPGDB_GPUPROJECTIONBUILDER_HPP
