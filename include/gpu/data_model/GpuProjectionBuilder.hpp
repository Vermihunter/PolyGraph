/**
 * @file include/gpu/data_model/GpuProjectionBuilder.hpp
 * @brief Build a cuGraph projection from UUID-only host inputs.
 */

#ifndef POLYGRAPH_GPUPROJECTIONBUILDER_HPP
#define POLYGRAPH_GPUPROJECTIONBUILDER_HPP

#include "gpu/data_model/CsrGraphProjection.hpp"
#include "gpu/GpuTransfer.hpp"
#include "events/Events.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cuda_runtime.h>
#include <cugraph/graph.hpp>
#include <cugraph/graph_functions.hpp>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <raft/core/handle.hpp>
#include <rmm/device_uvector.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace polygraph {

static inline void gpu_cuda_check(const cudaError_t error, const char *where) {
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("CUDA error at ") + where + ": " + cudaGetErrorString(error));
    }
}

class GpuProjectionBuilder {
  public:
    template <typename vertex_t, typename edge_t>
    static std::shared_ptr<CsrGraphProjection<vertex_t, edge_t>>
    from_prebuilt_dense_edgelist(raft::handle_t &handle, std::vector<GlobalId> dense_to_global,
                                 std::vector<vertex_t> dense_sources,
                                 std::vector<vertex_t> dense_destinations,
                                 const std::uint64_t graph_version, const int device_id = 0,
                                 GpuExecutionConfig execution_config = {}) {
        CudaDeviceGuard device_guard(device_id);
        const auto build_started = std::chrono::steady_clock::now();
        EVENT_SCOPE_INFO(gpu_load, "GPU reusable projection load",
                         {"vertices", dense_to_global.size()}, {"adjacency_pairs", dense_sources.size()});
        if (dense_sources.size() != dense_destinations.size()) {
            throw std::runtime_error("Prebuilt GPU source/destination arrays have different sizes");
        }
        if (dense_to_global.empty()) {
            throw std::runtime_error("Cannot build cuGraph projection: no vertices");
        }
        if (dense_sources.size() > static_cast<std::size_t>(std::numeric_limits<edge_t>::max())) {
            throw std::overflow_error("GPU projection contains more adjacency pairs than its edge type can represent");
        }

        // The cache builder assigns dense IDs in exactly the sorted GlobalId order
        // used by DenseIdMapper. No runtime sort, deduplication or binary-search
        // renumbering is necessary.
        auto mapper = DenseIdMapper<vertex_t>::from_sorted_unique(std::move(dense_to_global));
        const std::size_t vertex_count = mapper.size();
        const std::size_t edge_count = dense_sources.size();
        auto stream = handle.get_stream();

        std::vector<vertex_t> host_vertices(vertex_count);
        std::iota(host_vertices.begin(), host_vertices.end(), vertex_t{0});

        std::unique_ptr<ScopedHostRegistration> source_registration;
        std::unique_ptr<ScopedHostRegistration> destination_registration;
        std::unique_ptr<ScopedHostRegistration> vertex_registration;
        if (execution_config.pin_projection_build_inputs) {
            if (!dense_sources.empty()) {
                source_registration = std::make_unique<ScopedHostRegistration>(
                    dense_sources.data(), dense_sources.size() * sizeof(vertex_t));
            }
            if (!dense_destinations.empty()) {
                destination_registration = std::make_unique<ScopedHostRegistration>(
                    dense_destinations.data(), dense_destinations.size() * sizeof(vertex_t));
            }
            if (!host_vertices.empty()) {
                vertex_registration = std::make_unique<ScopedHostRegistration>(
                    host_vertices.data(), host_vertices.size() * sizeof(vertex_t));
            }
        }

        auto device_sources = rmm::device_uvector<vertex_t>(edge_count, stream);
        auto device_destinations = rmm::device_uvector<vertex_t>(edge_count, stream);
        auto device_vertices = rmm::device_uvector<vertex_t>(vertex_count, stream);
        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.projection.prebuilt_h2d_enqueue",
                             {"vertex_bytes", vertex_count * sizeof(vertex_t)},
                             {"edge_input_bytes", edge_count * 2U * sizeof(vertex_t)});
            if (edge_count > 0) {
                gpu_cuda_check(cudaMemcpyAsync(device_sources.data(), dense_sources.data(),
                                               edge_count * sizeof(vertex_t), cudaMemcpyHostToDevice, stream),
                               "copy prebuilt projection sources");
                gpu_cuda_check(cudaMemcpyAsync(device_destinations.data(), dense_destinations.data(),
                                               edge_count * sizeof(vertex_t), cudaMemcpyHostToDevice, stream),
                               "copy prebuilt projection destinations");
            }
            gpu_cuda_check(cudaMemcpyAsync(device_vertices.data(), host_vertices.data(),
                                           vertex_count * sizeof(vertex_t), cudaMemcpyHostToDevice, stream),
                           "copy prebuilt projection vertices");
            gpu_cuda_check(cudaStreamSynchronize(stream), "synchronize prebuilt projection input copy");
        }
        source_registration.reset();
        destination_registration.reset();
        vertex_registration.reset();

        // Release potentially hundreds of MiB of host COO before cuGraph allocates
        // its construction workspace and final CSR. This materially reduces peak
        // host memory and leaves only device COO + graph construction state.
        dense_sources.clear();
        dense_sources.shrink_to_fit();
        dense_destinations.clear();
        dense_destinations.shrink_to_fit();
        host_vertices.clear();
        host_vertices.shrink_to_fit();

        cugraph::graph_properties_t graph_properties{false, false};
        std::vector<cugraph::arithmetic_device_uvector_t> edge_properties;
        using projection_t = CsrGraphProjection<vertex_t, edge_t>;
        auto [graph, unused_edge_properties, unused_renumber_map] =
                cugraph::create_graph_from_edgelist<vertex_t, edge_t, projection_t::store_transposed,
                                                    projection_t::multi_gpu>(
                        handle, std::optional<rmm::device_uvector<vertex_t>>(std::move(device_vertices)),
                        std::move(device_sources), std::move(device_destinations), std::move(edge_properties),
                        graph_properties, false, std::nullopt, std::nullopt, false);
        (void) unused_edge_properties;
        (void) unused_renumber_map;
        handle.sync_stream();

        GpuProjectionStats stats{
                .vertex_count = vertex_count,
                .input_edge_count = edge_count,
                .raw_adjacency_pair_count = edge_count,
                .unique_adjacency_pair_count = edge_count,
                .duplicate_adjacency_pair_count = 0,
                .host_id_mapping_required_bytes = mapper.required_bytes(),
                .host_id_mapping_allocated_bytes = mapper.allocated_bytes(),
                .device_vertex_input_bytes = vertex_count * sizeof(vertex_t),
                .device_edge_input_bytes = edge_count * 2U * sizeof(vertex_t),
                .build_duration_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - build_started).count(),
                .mirrored_for_undirected_expansion = false,
        };
        return std::make_shared<projection_t>(handle, std::move(graph), std::move(mapper), graph_version,
                                              std::move(stats), device_id, std::move(execution_config));
    }

    template <typename vertex_t, typename edge_t>
    static std::shared_ptr<CsrGraphProjection<vertex_t, edge_t>>
    from_edgelist(raft::handle_t &handle, std::vector<GlobalId> vertex_ids,
                  std::vector<std::pair<GlobalId, GlobalId>> edges, const std::uint64_t graph_version,
                  const bool mirror_edges_for_undirected_expansion = false, const int device_id = 0,
                  GpuExecutionConfig execution_config = {}) {
        CudaDeviceGuard device_guard(device_id);
        const auto build_started = std::chrono::steady_clock::now();
        EVENT_SCOPE_INFO(gpu_load, "GPU data load", {"vertices", vertex_ids.size()}, {"edges", edges.size()},
                         {"mirror_edges", mirror_edges_for_undirected_expansion});
        auto stream = handle.get_stream();

        // Endpoints are accepted even when the data file omitted their explicit node
        // record from the GPU placement. Only this sorted UUID vector survives build.
        {
            EVENT_SCOPE_INFO(endpoint_span, "cugraph.projection.collect_endpoint_ids",
                             {"explicit_vertices", vertex_ids.size()}, {"edges", edges.size()});
            vertex_ids.reserve(vertex_ids.size() + edges.size() * 2);
            for (const auto &[source, destination]: edges) {
                vertex_ids.push_back(source);
                vertex_ids.push_back(destination);
            }
            EVENT_SCOPE_INFO_FIELD(endpoint_span, "candidate_vertex_ids", vertex_ids.size());
        }

        auto mapper = [&]() {
            EVENT_SCOPE_INFO(mapper_span, "cugraph.projection.build_dense_id_mapper",
                             {"candidate_vertex_ids", vertex_ids.size()});
            DenseIdMapper<vertex_t> built_mapper(std::move(vertex_ids));
            EVENT_SCOPE_INFO_FIELD(mapper_span, "unique_vertices", built_mapper.size());
            EVENT_SCOPE_INFO_FIELD(mapper_span, "required_bytes", built_mapper.required_bytes());
            EVENT_SCOPE_INFO_FIELD(mapper_span, "allocated_bytes", built_mapper.allocated_bytes());
            return built_mapper;
        }();

        if (mapper.size() == 0) {
            throw std::runtime_error("Cannot build cuGraph projection: no vertices");
        }

        const std::size_t input_edge_count = edges.size();
        std::vector<std::pair<vertex_t, vertex_t>> dense_edges;
        {
            EVENT_SCOPE_INFO(convert_span, "cugraph.projection.global_to_dense_edges",
                             {"input_edges", input_edge_count},
                             {"mirror_edges", mirror_edges_for_undirected_expansion});
            dense_edges.reserve(edges.size() * (mirror_edges_for_undirected_expansion ? 2U : 1U));
            for (const auto &[source, destination]: edges) {
                const vertex_t dense_source = mapper.at(source);
                const vertex_t dense_destination = mapper.at(destination);
                dense_edges.emplace_back(dense_source, dense_destination);
                if (mirror_edges_for_undirected_expansion && dense_source != dense_destination) {
                    dense_edges.emplace_back(dense_destination, dense_source);
                }
            }
            EVENT_SCOPE_INFO_FIELD(convert_span, "raw_adjacency_pairs", dense_edges.size());
        }
        edges.clear();
        edges.shrink_to_fit();

        const std::size_t raw_adjacency_pair_count = dense_edges.size();
        {
            EVENT_SCOPE_INFO(sort_span, "cugraph.projection.sort_deduplicate",
                             {"raw_adjacency_pairs", raw_adjacency_pair_count});
            std::ranges::sort(dense_edges);
            dense_edges.erase(std::unique(dense_edges.begin(), dense_edges.end()), dense_edges.end());
            EVENT_SCOPE_INFO_FIELD(sort_span, "unique_adjacency_pairs", dense_edges.size());
            EVENT_SCOPE_INFO_FIELD(sort_span, "duplicates_removed", raw_adjacency_pair_count - dense_edges.size());
        }
        const std::size_t unique_adjacency_pair_count = dense_edges.size();

        if (unique_adjacency_pair_count > static_cast<std::size_t>(std::numeric_limits<edge_t>::max())) {
            throw std::overflow_error("GPU projection contains more adjacency pairs than its edge type can represent");
        }

        std::vector<vertex_t> host_sources;
        std::vector<vertex_t> host_destinations;
        std::vector<vertex_t> host_vertices;
        {
            EVENT_SCOPE_INFO(host_span, "cugraph.projection.build_host_input_arrays",
                             {"vertices", mapper.size()}, {"adjacency_pairs", unique_adjacency_pair_count});
            host_sources.reserve(dense_edges.size());
            host_destinations.reserve(dense_edges.size());
            for (const auto &[source, destination]: dense_edges) {
                host_sources.push_back(source);
                host_destinations.push_back(destination);
            }
            host_vertices.resize(mapper.size());
            std::iota(host_vertices.begin(), host_vertices.end(), vertex_t{0});
            EVENT_SCOPE_INFO_FIELD(host_span, "source_bytes", host_sources.size() * sizeof(vertex_t));
            EVENT_SCOPE_INFO_FIELD(host_span, "destination_bytes", host_destinations.size() * sizeof(vertex_t));
            EVENT_SCOPE_INFO_FIELD(host_span, "vertex_bytes", host_vertices.size() * sizeof(vertex_t));
        }
        dense_edges.clear();
        dense_edges.shrink_to_fit();

        const std::size_t vertex_count = host_vertices.size();
        const std::size_t edge_count = host_sources.size();
        auto device_sources = [&]() {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.projection.device_allocate.sources",
                             {"adjacency_pairs", edge_count}, {"bytes", edge_count * sizeof(vertex_t)});
            return rmm::device_uvector<vertex_t>(edge_count, stream);
        }();
        auto device_destinations = [&]() {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.projection.device_allocate.destinations",
                             {"adjacency_pairs", edge_count}, {"bytes", edge_count * sizeof(vertex_t)});
            return rmm::device_uvector<vertex_t>(edge_count, stream);
        }();
        auto device_vertices = [&]() {
            EVENT_SCOPE_INFO(alloc_span, "cugraph.projection.device_allocate.vertices",
                             {"vertices", vertex_count}, {"bytes", vertex_count * sizeof(vertex_t)});
            return rmm::device_uvector<vertex_t>(vertex_count, stream);
        }();

        std::unique_ptr<ScopedHostRegistration> source_registration;
        std::unique_ptr<ScopedHostRegistration> destination_registration;
        std::unique_ptr<ScopedHostRegistration> vertex_registration;
        if (execution_config.pin_projection_build_inputs) {
            if (!host_sources.empty()) {
                source_registration = std::make_unique<ScopedHostRegistration>(
                    host_sources.data(), host_sources.size() * sizeof(vertex_t));
            }
            if (!host_destinations.empty()) {
                destination_registration = std::make_unique<ScopedHostRegistration>(
                    host_destinations.data(), host_destinations.size() * sizeof(vertex_t));
            }
            if (!host_vertices.empty()) {
                vertex_registration = std::make_unique<ScopedHostRegistration>(
                    host_vertices.data(), host_vertices.size() * sizeof(vertex_t));
            }
        }

        {
            EVENT_SCOPE_INFO(copy_span, "cugraph.projection.h2d_enqueue",
                             {"vertex_bytes", vertex_count * sizeof(vertex_t)},
                             {"edge_input_bytes", edge_count * 2U * sizeof(vertex_t)});
            if (edge_count > 0) {
                gpu_cuda_check(cudaMemcpyAsync(device_sources.data(), host_sources.data(), edge_count * sizeof(vertex_t),
                                               cudaMemcpyHostToDevice, stream),
                               "copy projection sources");
                gpu_cuda_check(cudaMemcpyAsync(device_destinations.data(), host_destinations.data(),
                                               edge_count * sizeof(vertex_t), cudaMemcpyHostToDevice, stream),
                               "copy projection destinations");
            }
            gpu_cuda_check(cudaMemcpyAsync(device_vertices.data(), host_vertices.data(), vertex_count * sizeof(vertex_t),
                                           cudaMemcpyHostToDevice, stream),
                           "copy projection vertices");
        }
        {
            EVENT_SCOPE_INFO(wait_span, "cugraph.projection.h2d_stream_wait");
            gpu_cuda_check(cudaStreamSynchronize(stream), "synchronize projection input copy");
        }
        source_registration.reset();
        destination_registration.reset();
        vertex_registration.reset();

        host_sources.clear();
        host_sources.shrink_to_fit();
        host_destinations.clear();
        host_destinations.shrink_to_fit();
        host_vertices.clear();
        host_vertices.shrink_to_fit();

        cugraph::graph_properties_t graph_properties{
                mirror_edges_for_undirected_expansion,
                false,
        };
        std::vector<cugraph::arithmetic_device_uvector_t> edge_properties;
        using projection_t = CsrGraphProjection<vertex_t, edge_t>;

        auto graph = [&]() {
            EVENT_SCOPE_INFO(create_span, "cugraph.projection.create_graph_from_edgelist",
                             {"vertices", vertex_count}, {"adjacency_pairs", edge_count});
            auto [created_graph, unused_edge_properties, unused_renumber_map] =
                    cugraph::create_graph_from_edgelist<vertex_t, edge_t, projection_t::store_transposed,
                                                        projection_t::multi_gpu>(
                            handle, std::optional<rmm::device_uvector<vertex_t>>(std::move(device_vertices)),
                            std::move(device_sources), std::move(device_destinations), std::move(edge_properties),
                            graph_properties, false, std::nullopt, std::nullopt, true);
            (void) unused_edge_properties;
            (void) unused_renumber_map;
            return created_graph;
        }();
        {
            EVENT_SCOPE_INFO(wait_span, "cugraph.projection.graph_build_stream_wait");
            handle.sync_stream();
        }

        GpuProjectionStats stats{
                .vertex_count = vertex_count,
                .input_edge_count = input_edge_count,
                .raw_adjacency_pair_count = raw_adjacency_pair_count,
                .unique_adjacency_pair_count = unique_adjacency_pair_count,
                .duplicate_adjacency_pair_count = raw_adjacency_pair_count - unique_adjacency_pair_count,
                .host_id_mapping_required_bytes = mapper.required_bytes(),
                .host_id_mapping_allocated_bytes = mapper.allocated_bytes(),
                .device_vertex_input_bytes = vertex_count * sizeof(vertex_t),
                .device_edge_input_bytes = edge_count * 2 * sizeof(vertex_t),
                .build_duration_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - build_started).count(),
                .mirrored_for_undirected_expansion = mirror_edges_for_undirected_expansion,
        };

        EVENT_SCOPE_INFO_FIELD(gpu_load, "unique_adjacency_pairs", unique_adjacency_pair_count);
        EVENT_SCOPE_INFO_FIELD(gpu_load, "duplicates_removed", stats.duplicate_adjacency_pair_count);
        EVENT_SCOPE_INFO_FIELD(gpu_load, "host_id_mapping_bytes", stats.host_id_mapping_required_bytes);
        EVENT_SCOPE_INFO_FIELD(gpu_load, "device_vertex_input_bytes", stats.device_vertex_input_bytes);
        EVENT_SCOPE_INFO_FIELD(gpu_load, "device_edge_input_bytes", stats.device_edge_input_bytes);
        EVENT_SCOPE_INFO_FIELD(gpu_load, "build_duration_seconds", stats.build_duration_seconds);
        return std::make_shared<projection_t>(handle, std::move(graph), std::move(mapper), graph_version,
                                              std::move(stats), device_id, std::move(execution_config));
    }
};

} // namespace polygraph

#endif // POLYGRAPH_GPUPROJECTIONBUILDER_HPP
