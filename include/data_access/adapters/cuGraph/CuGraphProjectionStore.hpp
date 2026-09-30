/**
 * @file include/data_access/adapters/cuGraph/CuGraphProjectionStore.hpp
 * @brief UUID-only cuGraph projection store.
 */

#ifndef POLYGRAPH_CUGRAPHPROJECTIONSTORE_HPP
#define POLYGRAPH_CUGRAPHPROJECTIONSTORE_HPP

#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "data_access/IGraphStore.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "domain/graph/TypeId.hpp"
#include "gpu/algorithms/CuGraphBfsExecutor.hpp"
#include "gpu/algorithms/CuGraphOneHopExpandExecutor.hpp"
#include "gpu/GpuBfsWorkspace.hpp"
#include "gpu/GpuTransfer.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "events/Events.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polygraph {

template <typename ProjectionT> class CuGraphProjectionStore final : public IGraphStore {
  public:
    CuGraphProjectionStore(BackendDescriptor descriptor, std::shared_ptr<ProjectionT> projection) :
        descriptor_(std::move(descriptor)), projection_(std::move(projection)) {
        if (!projection_) {
            throw std::invalid_argument("CuGraphProjectionStore requires a non-null projection");
        }
    }

    [[nodiscard]] BackendKind kind() const override { return BackendKind::CuGraph; }
    [[nodiscard]] const BackendDescriptor &descriptor() const override { return descriptor_; }

    [[nodiscard]] bool can_run_native_bfs(const NodeRef &source, const ExpandSpec &spec,
                                          const BfsOptions &options) const override {
        return options.allow_native_pushdown && descriptor_.capabilities.supports_native_bfs &&
               descriptor_.coverage.can_answer_whole_bfs(source, spec) && projection_->mapper().contains(source.global_id) &&
               supports_uuid_only_spec(spec);
    }

    BfsResult run_bfs(const NodeRef &source, const ExpandSpec &spec, const BfsOptions &options) override {
        EVENT_SCOPE_INFO(total_span, "cugraph.store.native_bfs", {"projection_vertices", projection_->vertex_count()});
        if (!can_run_native_bfs(source, spec, options)) {
            throw std::runtime_error("GPU projection cannot answer whole BFS for this source/spec");
        }
        auto result = CuGraphBfsExecutor::run(*projection_, source.global_id, options);
        EVENT_SCOPE_INFO_FIELD(total_span, "result_vertices", result.distance.size());
        return result;
    }

    std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                  const ExpandSpec &spec, const int depth,
                                                                  const BfsOptions &options) override {
        (void) options;
        validate_uuid_only_spec(spec);
        EVENT_SCOPE_INFO(total_span, "cugraph.get_minimal_neighbors", {"depth", depth},
                         {"frontier_size", frontier.size()}, {"projection_vertices", projection_->vertex_count()});

        std::vector<MinimalAdjacencyRow> rows;
        std::unordered_map<GlobalId, std::size_t> row_by_source;
        std::vector<GlobalId> gpu_source_ids;
        std::size_t projection_misses = 0;
        {
            EVENT_SCOPE_INFO(classify_span, "cugraph.frontier.classify", {"frontier_size", frontier.size()});
            rows.reserve(frontier.size());
            row_by_source.reserve(frontier.size());
            gpu_source_ids.reserve(frontier.size());

            for (const auto &source: frontier) {
                row_by_source.emplace(source.global_id, rows.size());
                rows.push_back(MinimalAdjacencyRow{.source = source, .neighbors = {}});
                if (projection_->mapper().contains(source.global_id)) {
                    gpu_source_ids.push_back(source.global_id);
                } else {
                    ++projection_misses;
                }
            }
            EVENT_SCOPE_INFO_FIELD(classify_span, "gpu_sources", gpu_source_ids.size());
            EVENT_SCOPE_INFO_FIELD(classify_span, "projection_misses", projection_misses);
            EVENT_SCOPE_INFO_FIELD(classify_span, "row_index_entries", row_by_source.size());
        }

        EVENT_SCOPE_INFO_FIELD(total_span, "gpu_sources", gpu_source_ids.size());
        EVENT_SCOPE_INFO_FIELD(total_span, "projection_misses", projection_misses);
        if (gpu_source_ids.empty()) {
            EVENT_SCOPE_INFO_FIELD(total_span, "sampled_edges", 0);
            EVENT_SCOPE_INFO_FIELD(total_span, "unique_neighbors", 0);
            return rows;
        }

        const BfsOptions one_hop_options{.max_depth = 1,
                                         .return_predecessors = false,
                                         .allow_native_pushdown = false,
                                         .allow_partial_acceleration = true};
        using vertex_t = typename ProjectionT::vertex_type;
        using edge_t = typename ProjectionT::edge_type;

        OneHopExpansionResult expanded;
        {
            EVENT_SCOPE_INFO(expand_span, "cugraph.frontier.expand", {"gpu_sources", gpu_source_ids.size()});
            expanded = CuGraphOneHopExpandExecutor::template run<vertex_t,
                                                                  edge_t,
                                                                  ProjectionT::multi_gpu,
                                                                  ProjectionT::store_transposed>(
                *projection_, gpu_source_ids, one_hop_options);
            EVENT_SCOPE_INFO_FIELD(expand_span, "sampled_edges", expanded.edges.size());
        }

        /*
        std::size_t unique_neighbors = 0;
        std::size_t duplicate_edges = 0;
        std::size_t unmatched_source_edges = 0;
        std::size_t max_row_degree = 0;
        {
            EVENT_SCOPE_INFO(assemble_span, "cugraph.rows.assemble_deduplicate", {"row_count", rows.size()},
                             {"sampled_edges", expanded.edges.size()});
            std::vector<std::unordered_set<GlobalId>> seen(rows.size());
            for (const auto &[source, destination]: expanded.edges) {
                const auto row = row_by_source.find(source);
                if (row == row_by_source.end()) {
                    ++unmatched_source_edges;
                    continue;
                }
                const std::size_t index = row->second;
                if (seen[index].insert(destination).second) {
                    rows[index].neighbors.push_back(NodeRef{.global_id = destination, .type = unresolved_type_id});
                    ++unique_neighbors;
                    if (rows[index].neighbors.size() > max_row_degree) {
                        max_row_degree = rows[index].neighbors.size();
                    }
                } else {
                    ++duplicate_edges;
                }
            }
            EVENT_SCOPE_INFO_FIELD(assemble_span, "unique_neighbors", unique_neighbors);
            EVENT_SCOPE_INFO_FIELD(assemble_span, "duplicate_edges", duplicate_edges);
            EVENT_SCOPE_INFO_FIELD(assemble_span, "unmatched_source_edges", unmatched_source_edges);
            EVENT_SCOPE_INFO_FIELD(assemble_span, "max_row_degree", max_row_degree);
        }
        */

        {
            std::size_t unmatched_source_edges = 0;
            EVENT_SCOPE_INFO(assemble_span, "cugraph.rows.assemble", {"row_count", rows.size()},
                             {"sampled_edges", expanded.edges.size()});

            for (const auto &[source, destination] : expanded.edges) {
                const auto row = row_by_source.find(source);

                if (row == row_by_source.end()) {
                    ++unmatched_source_edges;
                    continue;
                }

                rows[row->second].neighbors.push_back(NodeRef{
                    .global_id = destination,
                    .type = unresolved_type_id
                });
            }

            EVENT_SCOPE_INFO_FIELD(
                assemble_span,
                "unmatched_source_edges",
                unmatched_source_edges
            );
        }

        EVENT_SCOPE_INFO_FIELD(total_span, "sampled_edges", expanded.edges.size());
        /*
        EVENT_SCOPE_INFO_FIELD(total_span, "unique_neighbors", unique_neighbors);
        EVENT_SCOPE_INFO_FIELD(total_span, "duplicate_edges", duplicate_edges);
        EVENT_SCOPE_INFO_FIELD(total_span, "max_row_degree", max_row_degree);
        */
        return rows;
    }

    void stream_neighbor_candidates(std::span<const NodeRef> frontier, const ExpandSpec &spec, const int depth,
                                    const BfsOptions &options,
                                    const NeighborBatchConsumer &consumer) override {
        validate_uuid_only_spec(spec);
        EVENT_SCOPE_INFO(total_span, "cugraph.stream_neighbor_candidates", {"depth", depth},
                         {"frontier_size", frontier.size()},
                         {"projection_vertices", projection_->vertex_count()},
                         {"candidate_batch_size", options.streaming_candidate_batch_size});

        if (!consumer || frontier.empty()) {
            return;
        }

        std::vector<GlobalId> gpu_source_ids;
        gpu_source_ids.reserve(frontier.size());
        std::size_t projection_misses = 0;
        for (const auto &source: frontier) {
            if (projection_->mapper().contains(source.global_id)) {
                gpu_source_ids.push_back(source.global_id);
            } else {
                ++projection_misses;
            }
        }

        EVENT_SCOPE_INFO_FIELD(total_span, "gpu_sources", gpu_source_ids.size());
        EVENT_SCOPE_INFO_FIELD(total_span, "projection_misses", projection_misses);
        if (gpu_source_ids.empty()) {
            return;
        }

        using vertex_t = typename ProjectionT::vertex_type;
        using edge_t = typename ProjectionT::edge_type;
        std::shared_ptr<QueryVisitedContext> query_visited_context;
        std::unique_lock<std::mutex> query_visited_lock;
        if (projection_->execution_config().device_query_visited_filter &&
            projection_->execution_config().device_candidate_compaction &&
            options.scheduler_query_id != 0) {
            query_visited_context = acquire_query_visited_context(options.scheduler_query_id);
            query_visited_lock = std::unique_lock<std::mutex>(query_visited_context->mutex);
            auto stream = projection_->handle().get_stream();
            if (depth == 0 || depth < query_visited_context->last_depth) {
                query_visited_context->workspace.reset(stream);
            }
            query_visited_context->last_depth = depth;
        }

        CuGraphOneHopExpandExecutor::template stream_unique_destinations<vertex_t,
                                                                          edge_t,
                                                                          ProjectionT::multi_gpu,
                                                                          ProjectionT::store_transposed>(
                *projection_, gpu_source_ids, options.streaming_candidate_batch_size, consumer,
                query_visited_context ? &query_visited_context->workspace : nullptr);
    }

    std::vector<AdjacencyRow> get_neighbors_batch(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                   const int depth) override {
        EVENT_SCOPE_INFO(total_span, "cugraph.get_neighbors", {"depth", depth}, {"frontier_size", frontier.size()});
        const BfsOptions options{.max_depth = 1,
                                 .return_predecessors = false,
                                 .allow_native_pushdown = false,
                                 .allow_partial_acceleration = true};
        auto minimal = get_minimal_neighbors_batch(frontier, spec, depth, options);

        std::vector<AdjacencyRow> rich;
        std::size_t neighbor_count = 0;
        {
            EVENT_SCOPE_INFO(convert_span, "cugraph.rows.minimal_to_rich", {"row_count", minimal.size()});
            rich.reserve(minimal.size());
            for (auto &row: minimal) {
                AdjacencyRow converted{.source = row.source, .neighbors = {}};
                converted.neighbors.reserve(row.neighbors.size());
                neighbor_count += row.neighbors.size();
                for (const auto &neighbor: row.neighbors) {
                    converted.neighbors.push_back(NeighborRef{.vertex = neighbor, .via_edge = std::nullopt});
                }
                rich.push_back(std::move(converted));
            }
            EVENT_SCOPE_INFO_FIELD(convert_span, "neighbors", neighbor_count);
        }
        EVENT_SCOPE_INFO_FIELD(total_span, "neighbors", neighbor_count);
        return rich;
    }

    [[nodiscard]] const ProjectionT &projection() const noexcept { return *projection_; }

  private:
    using projection_vertex_t = typename ProjectionT::vertex_type;

    struct QueryVisitedContext {
        QueryVisitedContext(const std::uint64_t id,
                            const std::size_t vertex_count,
                            const rmm::cuda_stream_view stream) :
            query_id(id), workspace(vertex_count, stream) {}

        std::uint64_t query_id{0};
        int last_depth{-1};
        std::mutex mutex;
        GpuQueryVisitedWorkspace<projection_vertex_t> workspace;
    };

    std::shared_ptr<QueryVisitedContext> acquire_query_visited_context(const std::uint64_t query_id) {
        std::scoped_lock lock(query_visited_contexts_mutex_);
        if (const auto it = query_visited_contexts_.find(query_id); it != query_visited_contexts_.end()) {
            return it->second;
        }

        // The CPU/global visited map remains authoritative, so evicting an old
        // GPU context can only reduce this optimization's effectiveness; it
        // cannot change BFS correctness. This bounds extra GPU memory under
        // concurrent HTTP workloads.
        constexpr std::size_t max_contexts = 8;
        if (query_visited_contexts_.size() >= max_contexts) {
            query_visited_contexts_.erase(query_visited_contexts_.begin());
        }

        CudaDeviceGuard device_guard(projection_->device_id());
        auto context = std::make_shared<QueryVisitedContext>(
                query_id, projection_->vertex_count(), projection_->handle().get_stream());
        query_visited_contexts_.emplace(query_id, context);
        return context;
    }

    [[nodiscard]] static bool supports_uuid_only_spec(const ExpandSpec &spec) noexcept {
        return spec.direction != Direction::Directed && spec.allowed_edge_types.empty() &&
               spec.allowed_target_node_types.empty();
    }

    static void validate_uuid_only_spec(const ExpandSpec &spec) {
        if (spec.direction == Direction::Directed) {
            throw std::runtime_error(
                    "The compact GPU projection is UUID-only and materialized for both/undirected expansion; "
                    "directed expansion requires a separate directed projection");
        }
        if (!spec.allowed_edge_types.empty() || !spec.allowed_target_node_types.empty()) {
            throw std::runtime_error(
                    "The compact GPU projection does not retain node/edge attributes, so logical type filters cannot "
                    "be evaluated on GPU");
        }
    }

    BackendDescriptor descriptor_;
    std::shared_ptr<ProjectionT> projection_;
    std::mutex query_visited_contexts_mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<QueryVisitedContext>> query_visited_contexts_;
};

} // namespace polygraph

#endif // POLYGRAPH_CUGRAPHPROJECTIONSTORE_HPP
