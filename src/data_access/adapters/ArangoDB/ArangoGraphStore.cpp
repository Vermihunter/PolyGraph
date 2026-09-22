/**
 * @file src/data_access/adapters/ArangoDB/ArangoGraphStore.cpp
 * @brief Implementation for arango graph store in the graph-store access module.
 */

#include "data_access/adapters/ArangoDB/ArangoGraphStore.hpp"

#include "data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp"

#include "data_model/Edge.hpp"
#include "data_model/Node.hpp"
#include "data_model/Value.hpp"
#include "query/query_types/GetNeighborsBatchQuery.hpp"

#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace polygraph {

    ArangoGraphStore::ArangoGraphStore(BackendDescriptor descriptor, std::shared_ptr<QueryVisitor> visitor) :
        descriptor_(std::move(descriptor)), visitor_(std::move(visitor)) {
        if (!visitor_) {
            throw std::invalid_argument("ArangoGraphStore requires a query visitor");
        }
    }

    BackendKind ArangoGraphStore::kind() const { return BackendKind::Arango; }
    const BackendDescriptor &ArangoGraphStore::descriptor() const { return descriptor_; }

    query_result_t ArangoGraphStore::execute(const Query &query) { return query.accept(*visitor_); }

    std::vector<AdjacencyRow> ArangoGraphStore::get_neighbors_batch(std::span<const NodeRef> frontier,
                                                                    const ExpandSpec &spec, int) {
        if (frontier.empty()) {
            return {};
        }
        auto raw =
                visitor_->visit(GetNeighborsBatchQuery(std::vector<NodeRef>(frontier.begin(), frontier.end()), spec));
        return raw ? parse_adjacency_rows(*raw) : std::vector<AdjacencyRow>{};
    }

    std::vector<MinimalAdjacencyRow> ArangoGraphStore::get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                                   const ExpandSpec &spec, int depth,
                                                                                   const BfsOptions &options) {
        if (frontier.empty()) {
            return {};
        }

        if (auto *visitor = dynamic_cast<ArangoQueryVisitor *>(visitor_.get())) {
            const BackendRequestScheduling scheduling{.query_id = options.scheduler_query_id,
                                                      .max_in_flight = options.arango_max_concurrent_requests,
                                                      .priority = options.backend_scheduler_priority,
                                                      .weight = options.backend_scheduler_weight};
            return visitor->get_minimal_neighbors_batch_fast(
                    frontier, spec, depth, options.neighbor_batch_size, options.arango_max_concurrent_requests,
                    options.arango_request_strategy, options.arango_single_request_max_frontier,
                    options.backend_request_retries, options.backend_retry_delay_ms, scheduling);
        }

        return IGraphStore::get_minimal_neighbors_batch(frontier, spec, depth, options);
    }

    void ArangoGraphStore::stream_neighbor_candidates(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                      int depth, const BfsOptions &options,
                                                      const NeighborBatchConsumer &consumer) {
        if (!consumer || frontier.empty()) {
            return;
        }

        if (auto *visitor = dynamic_cast<ArangoQueryVisitor *>(visitor_.get())) {
            const BackendRequestScheduling scheduling{.query_id = options.scheduler_query_id,
                                                      .max_in_flight = options.arango_max_concurrent_requests,
                                                      .priority = options.backend_scheduler_priority,
                                                      .weight = options.backend_scheduler_weight};
            visitor->stream_neighbor_candidates_fast(
                    frontier, spec, depth, options.neighbor_batch_size, options.arango_max_concurrent_requests,
                    options.arango_request_strategy, options.arango_single_request_max_frontier,
                    options.backend_request_retries, options.backend_retry_delay_ms,
                    options.streaming_candidate_batch_size, consumer, scheduling);
            return;
        }

        IGraphStore::stream_neighbor_candidates(frontier, spec, depth, options, consumer);
    }

    NodeRef ArangoGraphStore::node_to_ref(const Node &node) {
        return NodeRef{.global_id = node.global_id, .type = node.type};
    }

    EdgeRef ArangoGraphStore::edge_to_ref(const Edge &edge) {
        return EdgeRef{.global_id = edge.global_id, .type = edge.type, .from = edge.src, .to = edge.dst};
    }

    std::vector<AdjacencyRow> ArangoGraphStore::parse_adjacency_rows(const QueryResult &raw) {
        std::vector<AdjacencyRow> rows;
        std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> source_to_row;

        for (const auto &record: raw.data) {
            if (!record) {
                continue;
            }
            if (record->fields.size() < 2 || !record->fields[0].is<Node>() || !record->fields[1].is<Node>()) {
                throw std::runtime_error("Arango adjacency row requires source and neighbor Node fields");
            }

            const NodeRef source = node_to_ref(record->fields[0].as<Node>());
            const NodeRef neighbor = node_to_ref(record->fields[1].as<Node>());
            std::optional<EdgeRef> edge;
            if (record->fields.size() >= 3 && !record->fields[2].is_null()) {
                if (!record->fields[2].is<Edge>()) {
                    throw std::runtime_error("Arango adjacency row field[2] must be Edge");
                }
                edge = edge_to_ref(record->fields[2].as<Edge>());
            }

            const auto [iterator, inserted] = source_to_row.emplace(source.global_id, rows.size());
            if (inserted) {
                rows.push_back(AdjacencyRow{.source = source, .neighbors = {}});
            }
            rows[iterator->second].neighbors.push_back(NeighborRef{.vertex = neighbor, .via_edge = edge});
        }
        return rows;
    }

} // namespace polygraph
