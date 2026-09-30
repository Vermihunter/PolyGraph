/**
 * @file include/gpu/data_model/CsrGraphProjection.hpp
 * @brief Owning cuGraph projection plus compact host ID mapping and build statistics.
 */

#ifndef POLYGRAPH_CSRGRAPHPROJECTION_HPP
#define POLYGRAPH_CSRGRAPHPROJECTION_HPP

#include "gpu/data_model/DenseIdMapper.hpp"
#include "gpu/GpuExecutionConfig.hpp"
#include "gpu/GpuBfsWorkspace.hpp"

#include <cstddef>
#include <cstdint>
#include <cugraph/graph.hpp>
#include <raft/core/handle.hpp>
#include <memory>
#include <mutex>
#include <utility>

namespace polygraph {

struct GpuProjectionStats {
    std::size_t vertex_count = 0;
    std::size_t input_edge_count = 0;
    std::size_t raw_adjacency_pair_count = 0;
    std::size_t unique_adjacency_pair_count = 0;
    std::size_t duplicate_adjacency_pair_count = 0;
    std::size_t host_id_mapping_required_bytes = 0;
    std::size_t host_id_mapping_allocated_bytes = 0;
    std::size_t device_vertex_input_bytes = 0;
    std::size_t device_edge_input_bytes = 0;
    double build_duration_seconds = 0.0;
    bool mirrored_for_undirected_expansion = false;
};

template <typename vertex_t, typename edge_t> class CsrGraphProjection {
  public:
    using vertex_type = vertex_t;
    using edge_type = edge_t;

    static constexpr bool store_transposed = false;
    static constexpr bool multi_gpu = false;

    using graph_t = cugraph::graph_t<vertex_t, edge_t, store_transposed, multi_gpu>;

    CsrGraphProjection(raft::handle_t &handle, graph_t graph, DenseIdMapper<vertex_t> mapper,
                       const std::uint64_t graph_version, GpuProjectionStats stats,
                       const int device_id = 0, GpuExecutionConfig execution_config = {}) :
        handle_(&handle), graph_(std::move(graph)), mapper_(std::move(mapper)), graph_version_(graph_version),
        stats_(std::move(stats)), device_id_(device_id), execution_config_(std::move(execution_config)) {}

    [[nodiscard]] raft::handle_t &handle() { return *handle_; }
    [[nodiscard]] const raft::handle_t &handle() const { return *handle_; }
    [[nodiscard]] auto view() const { return graph_.view(); }
    [[nodiscard]] const DenseIdMapper<vertex_t> &mapper() const { return mapper_; }
    [[nodiscard]] DenseIdMapper<vertex_t> &mapper() { return mapper_; }
    [[nodiscard]] std::size_t vertex_count() const { return mapper_.size(); }
    [[nodiscard]] std::uint64_t graph_version() const { return graph_version_; }
    [[nodiscard]] const GpuProjectionStats &stats() const noexcept { return stats_; }
    [[nodiscard]] int device_id() const noexcept { return device_id_; }
    [[nodiscard]] const GpuExecutionConfig &execution_config() const noexcept { return execution_config_; }

    [[nodiscard]] std::mutex &native_bfs_workspace_mutex() noexcept { return native_bfs_workspace_mutex_; }

    NativeGpuBfsWorkspace<vertex_t> &native_bfs_workspace() {
        if (!native_bfs_workspace_) {
            native_bfs_workspace_ = std::make_unique<NativeGpuBfsWorkspace<vertex_t>>(
                    vertex_count(), handle().get_stream());
        }
        return *native_bfs_workspace_;
    }

  private:
    raft::handle_t *handle_;
    graph_t graph_;
    DenseIdMapper<vertex_t> mapper_;
    std::uint64_t graph_version_;
    GpuProjectionStats stats_;
    int device_id_{0};
    GpuExecutionConfig execution_config_{};
    std::mutex native_bfs_workspace_mutex_;
    std::unique_ptr<NativeGpuBfsWorkspace<vertex_t>> native_bfs_workspace_;
};

} // namespace polygraph

#endif // POLYGRAPH_CSRGRAPHPROJECTION_HPP
