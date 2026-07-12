/**
 * @file include/gpu/data_model/CsrGraphProjection.hpp
 * @brief Declarations for csr graph projection in the GPU acceleration module.
 */

#ifndef CPGDB_CSRGRAPHPROJECTION_HPP
#define CPGDB_CSRGRAPHPROJECTION_HPP

#include "gpu/data_model/DenseIdMapper.hpp"

#include <cstdint>
#include <cugraph/graph.hpp>
#include <raft/core/handle.hpp>
#include <utility>

namespace cpgdb {
/*
 * Owns the cuGraph graph object plus the dense-id mapper.
 *
 * Important lifetime rule:
 *   The raft::handle_t passed to this object must outlive the projection.
 *   Prefer storing the handle in DatabaseContext / a GpuRuntime object instead
 *   of creating it as a short-lived local variable.
 */
template <typename vertex_t, typename edge_t> class CsrGraphProjection {
  public:
    using vertex_type = vertex_t;
    using edge_type = edge_t;

    static constexpr bool store_transposed = false;
    static constexpr bool multi_gpu = false;

    using graph_t = cugraph::graph_t<vertex_t, edge_t, store_transposed, multi_gpu>;

    CsrGraphProjection(raft::handle_t& handle,
                       graph_t graph,
                       DenseIdMapper<vertex_t> mapper,
                       std::uint64_t graph_version)
        : handle_(&handle)
        , graph_(std::move(graph))
        , mapper_(std::move(mapper))
        , graph_version_(graph_version) {}

    raft::handle_t& handle() {
        return *handle_;
    }

    const raft::handle_t& handle() const {
        return *handle_;
    }

    auto view() const {
        return graph_.view();
    }

    const DenseIdMapper<vertex_t>& mapper() const {
        return mapper_;
    }

    DenseIdMapper<vertex_t>& mapper() {
        return mapper_;
    }

    std::size_t vertex_count() const {
        return mapper_.size();
    }

    std::uint64_t graph_version() const {
        return graph_version_;
    }

  private:
    raft::handle_t* handle_;
    graph_t graph_;
    DenseIdMapper<vertex_t> mapper_;
    std::uint64_t graph_version_;
};

} // namespace cpgdb

#endif // CPGDB_CSRGRAPHPROJECTION_HPP
