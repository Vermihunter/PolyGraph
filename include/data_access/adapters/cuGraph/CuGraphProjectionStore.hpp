#ifndef CPGDB_CUGRAPHPROJECTIONSTORE_HPP
#define CPGDB_CUGRAPHPROJECTIONSTORE_HPP

#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "data_access/IGraphStore.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "gpu/algorithms/CuGraphBfsExecutor.hpp"
#include "gpu/algorithms/CuGraphOneHopExpandExecutor.hpp"
#include "gpu/data_model/HostAdjacencyIndex.hpp"
#include "infrastructure/BackendDescriptor.hpp"

template <typename ProjectionT>
class CuGraphProjectionStore final : public IGraphStore {
public:
    CuGraphProjectionStore(BackendDescriptor descriptor,
                           std::shared_ptr<ProjectionT> projection,
                           std::shared_ptr<HostAdjacencyIndex> host_index = nullptr)
        : descriptor_(std::move(descriptor)),
          projection_(std::move(projection)),
          host_index_(std::move(host_index)) {
        if (!projection_) {
            throw std::invalid_argument(
                "CuGraphProjectionStore requires a non-null projection"
            );
        }
    }

    BackendKind kind() const override {
        return BackendKind::CuGraph;
    }

    const BackendDescriptor& descriptor() const override {
        return descriptor_;
    }

    bool can_run_native_bfs(const NodeRef& source,
                            const ExpandSpec& spec,
                            const BfsOptions& options) const override {
        if (!options.allow_native_pushdown) {
            return false;
        }

        if (!descriptor_.capabilities.supports_native_bfs) {
            return false;
        }

        if (!descriptor_.coverage.can_answer_whole_bfs(source, spec)) {
            return false;
        }

        return projection_->mapper().contains(source.global_id);
    }

    BfsResult run_bfs(const NodeRef& source,
                      const ExpandSpec& spec,
                      const BfsOptions& options) override {
        if (!can_run_native_bfs(source, spec, options)) {
            throw std::runtime_error(
                "GPU projection cannot answer whole BFS for this source/spec"
            );
        }

        /*
        return CuGraphOneHopExpandExecutor::run_as_union_bfs_result(
            *projection_,
            source.global_id,
            options);
        */

        return CuGraphBfsExecutor::run(
            *projection_,
            source.global_id,
            options
        );
    }

    /*
    std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec) override {
        if (!host_index_) {
            throw std::runtime_error(
                "CuGraphProjectionStore has no host adjacency index for partial expansion"
            );
        }

        return host_index_->get_neighbors_batch(frontier, spec);
    }
    */

    std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec) override {
        if (!host_index_) {
            throw std::runtime_error(
                "CuGraphProjectionStore requires HostAdjacencyIndex to hydrate GPU expansion results"
            );
        }

        std::vector<AdjacencyRow> rows;
        rows.reserve(frontier.size());

        std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>> row_by_source;
        std::vector<GlobalId> gpu_source_ids;
        gpu_source_ids.reserve(frontier.size());

        for (const auto& source : frontier) {
            row_by_source.emplace(source.global_id, rows.size());
            rows.push_back(AdjacencyRow{
                .source = source,
                .neighbors = {}
            });

            if (projection_->mapper().contains(source.global_id)) {
                gpu_source_ids.push_back(source.global_id);
            }
        }

        if (gpu_source_ids.empty()) {
            return rows;
        }

        BfsOptions one_hop_options{
            .max_depth = 1,
            .return_predecessors = false,
            .allow_native_pushdown = false,
            .allow_partial_acceleration = true
        };

        using vertex_t = typename ProjectionT::vertex_type;
        using edge_t = typename ProjectionT::edge_type;

        OneHopExpansionResult expanded =
            CuGraphOneHopExpandExecutor::template run<
                vertex_t,
                edge_t,
                ProjectionT::multi_gpu,
                ProjectionT::store_transposed
            >(
                *projection_,
                gpu_source_ids,
                one_hop_options
            );

        std::unordered_map<GlobalId, std::unordered_set<GlobalId, boost::hash<GlobalId>>, boost::hash<GlobalId>>
            seen_by_source;

        for (const auto& [source_id, destination_id] : expanded.edges) {
            auto row_it = row_by_source.find(source_id);
            if (row_it == row_by_source.end()) {
                continue;
            }

            auto hydrated_neighbor =
                host_index_->find_neighbor(source_id, destination_id, spec);

            if (!hydrated_neighbor.has_value()) {
                // The GPU graph may intentionally contain mirrored edges for
                // fast undirected expansion. The CPU metadata index is the
                // authority for edge direction, edge type and target type, so
                // invalid sampled candidates are discarded here.
                continue;
            }

            auto& seen = seen_by_source[source_id];
            if (!seen.insert(hydrated_neighbor->vertex.global_id).second) {
                continue;
            }

            rows[row_it->second].neighbors.push_back(*hydrated_neighbor);
        }

        return rows;
    }

private:
    BackendDescriptor descriptor_;
    std::shared_ptr<ProjectionT> projection_;
    std::shared_ptr<HostAdjacencyIndex> host_index_;
};

#endif // CPGDB_CUGRAPHPROJECTIONSTORE_HPP
