/**
 * @file src/server/services/TraversalService.cpp
 * @brief Implementation for traversal service in the server frontend module.
 */

#include "server/services/TraversalService.hpp"
#include "utilities/uuid.hpp"

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsExecutor.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "infrastructure/scheduling/BackendRequestScheduler.hpp"

#include <stdexcept>

namespace polygraph::services {
    namespace {

        [[nodiscard]] Direction parse_direction(const std::string &value) {
            if (value == "directed") {
                return Direction::Directed;
            }
            if (value == "undirected") {
                return Direction::Undirected;
            }
            if (value == "both") {
                return Direction::Both;
            }
            throw std::invalid_argument("Unknown BFS direction: " + value);
        }

        [[nodiscard]] ExpandSpec make_expand_spec(const dto::BfsRequest &request,
                                                  const domain::LogicalGraphSchema &schema) {
            ExpandSpec spec;
            spec.direction = parse_direction(request.direction);
            spec.return_edge_refs = request.includeEdges;
            spec.allowed_edge_types.reserve(request.allowedEdgeTypes.size());
            for (const auto &name: request.allowedEdgeTypes) {
                spec.allowed_edge_types.push_back(schema.edge_type_id(name));
            }
            spec.allowed_target_node_types.reserve(request.allowedTargetNodeTypes.size());
            for (const auto &name: request.allowedTargetNodeTypes) {
                spec.allowed_target_node_types.push_back(schema.node_type_id(name));
            }
            return spec;
        }

        [[nodiscard]] BfsOptions make_bfs_options(const dto::BfsRequest &request) {
            BfsOptions options{.max_depth = std::nullopt,
                               .return_predecessors = request.returnPredecessors,
                               .allow_native_pushdown = request.allowNativePushdown,
                               .allow_partial_acceleration = request.allowPartialAcceleration,
                               .neighbor_batch_size = request.neighborBatchSize,
                               .neo4j_max_concurrent_requests = request.neo4jMaxConcurrentRequests,
                               .arango_max_concurrent_requests = request.arangoMaxConcurrentRequests,
                               .arango_request_strategy = parse_arango_request_strategy(request.arangoRequestStrategy),
                               .arango_single_request_max_frontier = request.arangoSingleRequestMaxFrontier,
                               .streaming_candidate_batch_size = request.streamingCandidateBatchSize,
                               .redis_pipeline_batch_size = request.redisPipelineBatchSize,
                               .scheduler_query_id = request.queryExecutionId,
                               .backend_scheduler_priority = request.backendSchedulerPriority,
                               .backend_scheduler_weight = request.backendSchedulerWeight,
                               .backend_request_retries = request.backendRequestRetries,
                               .backend_retry_delay_ms = request.backendRetryDelayMs,
                               .thread_count = request.threadCount};
            if (request.maxDepth >= 0) {
                options.max_depth = static_cast<std::uint32_t>(request.maxDepth);
            }
            return options;
        }

    } // namespace

    TraversalService::TraversalService(DatabaseContext &context) noexcept :
        context_(context), expander_(*context.placement_catalog) {}

    BfsResult TraversalService::runBfs(const std::string &source_vertex_uuid, const dto::BfsRequest &request) const {
        const BfsExecutor bfs(*context_.placement_catalog);
        const NodeRef source = context_.node_ref(global_id_from_string(source_vertex_uuid));
        return bfs.run(source, make_expand_spec(request, *context_.schema), make_bfs_options(request));
    }

    std::vector<MinimalAdjacencyRow> TraversalService::getNeighbors(const std::string &source_vertex_uuid) const {
        const NodeRef source = context_.node_ref(global_id_from_string(source_vertex_uuid));
        const std::vector frontier{source};

        const ExpandSpec spec{};
        const BfsOptions options{.max_depth = 1,
                                 .return_predecessors = false,
                                 .allow_native_pushdown = false,
                                 .allow_partial_acceleration = true,
                                 .scheduler_query_id = next_backend_scheduler_query_id()};

        return expander_.expand_layer(frontier, spec, 0, options);
    }

    BfsResult TraversalService::streamBfs(const std::string &graph_id, const std::string &source_vertex_id,
                                          std::uint32_t max_depth, bool return_predecessors, bool allow_native_pushdown,
                                          bool allow_partial_acceleration,
                                          const std::function<void(const BfsLayerEvent &)> &on_layer) const {
        (void) graph_id;
        const BfsExecutor bfs(*context_.placement_catalog);
        const NodeRef source = context_.node_ref(global_id_from_string(source_vertex_id));
        return bfs.run_streaming(source, ExpandSpec{},
                                 BfsOptions{.max_depth = max_depth,
                                            .return_predecessors = return_predecessors,
                                            .allow_native_pushdown = allow_native_pushdown,
                                            .allow_partial_acceleration = allow_partial_acceleration},
                                 on_layer);
    }

} // namespace polygraph::services
