/**
 * @file src/data_access/adapters/Neo4J/Neo4jGraphStore.cpp
 * @brief Implementation for neo4j graph store in the graph-store access module.
 */

#include "data_access/adapters/Neo4J/Neo4jGraphStore.hpp"

#include "data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp"
#include "data_model/Edge.hpp"
#include "data_model/Node.hpp"
#include "data_model/Value.hpp"
#include "query/query_types/GetNeighborsBatchQuery.hpp"
#include "utilities/Profiling.hpp"

#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cpgdb {

    Neo4jGraphStore::Neo4jGraphStore(DatabaseContext &context, BackendDescriptor descriptor,
                                     std::shared_ptr<QueryVisitor> visitor) :
        context_(context), descriptor_(std::move(descriptor)), visitor_(std::move(visitor)) {
        if (!visitor_) {
            throw std::invalid_argument("Neo4jGraphStore requires a query visitor");
        }
    }

    BackendKind Neo4jGraphStore::kind() const { return BackendKind::Neo4j; }
    const BackendDescriptor &Neo4jGraphStore::descriptor() const { return descriptor_; }

    query_result_t Neo4jGraphStore::execute(const Query &query) { return query.accept(*visitor_); }

    std::vector<AdjacencyRow> Neo4jGraphStore::get_neighbors_batch(std::span<const NodeRef> frontier,
                                                                   const ExpandSpec &spec, int depth) {
        CPGDB_PROFILE_FUNCTION();
        if (frontier.empty()) {
            return {};
        }

        if (auto *visitor = dynamic_cast<Neo4JQueryVisitor *>(visitor_.get())) {
            CPGDB_PROFILE_SCOPE("Neo4jGraphStore: fast get_neighbors_batch");
            return visitor->get_neighbors_batch_fast(frontier, spec, depth);
        }

        std::vector<NodeRef> copied(frontier.begin(), frontier.end());
        auto raw = visitor_->visit(GetNeighborsBatchQuery(std::move(copied), spec));
        return raw ? parse_adjacency_rows(*raw, frontier) : std::vector<AdjacencyRow>{};
    }

    std::vector<MinimalAdjacencyRow> Neo4jGraphStore::get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                                  const ExpandSpec &spec, int depth,
                                                                                  const BfsOptions &options) {
        if (frontier.empty()) {
            return {};
        }

        if (auto *visitor = dynamic_cast<Neo4JQueryVisitor *>(visitor_.get())) {
            return visitor->get_minimal_neighbors_batch_fast(
                    frontier, spec, depth, options.neighbor_batch_size, options.neo4j_max_concurrent_requests,
                    options.backend_request_retries, options.backend_retry_delay_ms);
        }

        return IGraphStore::get_minimal_neighbors_batch(frontier, spec, depth, options);
    }

    BfsResult Neo4jGraphStore::run_bfs(const NodeRef &, const ExpandSpec &, const BfsOptions &) { return {}; }

    NodeRef Neo4jGraphStore::node_to_ref(const Node &node) const {
        if (const auto iterator = node.properties.find("remote-type");
            iterator != node.properties.end() && iterator->second && iterator->second->is<std::string>()) {
            return NodeRef{.global_id = node.global_id,
                           .type = context_.schema->node_type_id(iterator->second->as<std::string>())};
        }
        return NodeRef{.global_id = node.global_id, .type = node.type};
    }

    EdgeRef Neo4jGraphStore::edge_to_ref(const Edge &edge) {
        return EdgeRef{.global_id = edge.global_id, .type = edge.type, .from = edge.src, .to = edge.dst};
    }

    std::vector<AdjacencyRow> Neo4jGraphStore::parse_adjacency_rows(const QueryResult &raw,
                                                                    std::span<const NodeRef> frontier) const {
        if (frontier.empty() || raw.data.empty() || !raw.data.front() || raw.data.front()->fields.empty()) {
            return {};
        }
        return raw.data.front()->fields.size() == 1 ? parse_neighbor_only_rows(raw, frontier)
                                                    : parse_source_neighbor_rows(raw);
    }

    std::vector<AdjacencyRow> Neo4jGraphStore::parse_neighbor_only_rows(const QueryResult &raw,
                                                                        std::span<const NodeRef> frontier) const {
        if (frontier.size() != 1) {
            throw std::runtime_error("Neighbor-only Neo4j rows cannot identify sources for a batched frontier");
        }

        AdjacencyRow row{.source = frontier.front(), .neighbors = {}};
        row.neighbors.reserve(raw.data.size());
        for (const auto &record: raw.data) {
            if (!record || record->fields.empty()) {
                continue;
            }
            const Value &value = record->fields.front();
            if (!value.is<Node>()) {
                throw std::runtime_error("Expected neighbor-only field[0] to be Node");
            }
            row.neighbors.push_back(NeighborRef{.vertex = node_to_ref(value.as<Node>()), .via_edge = std::nullopt});
        }
        return {std::move(row)};
    }

    std::vector<AdjacencyRow> Neo4jGraphStore::parse_source_neighbor_rows(const QueryResult &raw) const {
        std::vector<AdjacencyRow> rows;
        std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> source_to_row;

        for (const auto &record: raw.data) {
            if (!record) {
                continue;
            }
            if (record->fields.size() < 2 || !record->fields[0].is<Node>() || !record->fields[1].is<Node>()) {
                throw std::runtime_error("Expected source and neighbor Node fields");
            }

            const NodeRef source = node_to_ref(record->fields[0].as<Node>());
            const NodeRef neighbor = node_to_ref(record->fields[1].as<Node>());
            std::optional<EdgeRef> edge;
            if (record->fields.size() >= 3 && !record->fields[2].is_null()) {
                if (!record->fields[2].is<Edge>()) {
                    throw std::runtime_error("Expected field[2] to be Edge");
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

} // namespace cpgdb
