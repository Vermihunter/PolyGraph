//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_NEO4JGRAPHSTORE_HPP
#define CPGDB_NEO4JGRAPHSTORE_HPP

#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container_hash/hash.hpp>

#include "Neo4JQueryVisitor.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "data_access/IGraphStore.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "data_model/Edge.hpp"
#include "data_model/Node.hpp"
#include "data_model/Value.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "query/QueryVisitor.hpp"
#include "query/query_types/GetNeighborsBatchQuery.hpp"
#include "utilities/Profiling.hpp"

class Neo4jGraphStore final : public IGraphStore {
public:
    Neo4jGraphStore(DatabaseContext& ctx,
                    BackendDescriptor descriptor,
                    std::shared_ptr<QueryVisitor>& visitor)
        : ctx_(ctx),
          descriptor_(std::move(descriptor)),
          visitor_(std::move(visitor)) {}

    [[nodiscard]] BackendKind kind() const override {
        return BackendKind::Neo4j;
    }

    const BackendDescriptor& descriptor() const override {
        return descriptor_;
    }

    std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec, int depth) override {
        CPGDB_PROFILE_FUNCTION();

        if (frontier.empty()) {
            return {};
        }

        /*
         * Important:
         *
         * Use the Neo4J-specific fast path.
         *
         * The old generic path builds:
         *   QueryResult -> Record -> Value -> Node -> Edge
         *
         * for every returned edge row. That is fine for generic queries, but it is
         * too expensive for BFS.
         */
        if (auto* neo4j_visitor =
                dynamic_cast<Neo4JQueryVisitor*>(visitor_.get())) {
            CPGDB_PROFILE_SCOPE("Neo4jGraphStore: fast get_neighbors_batch");
            return neo4j_visitor->get_neighbors_batch_fast(frontier, spec, depth);
        }

        /*
         * Fallback for non-standard visitors.
         */
        //CPGDB_PROFILE_SCOPE("Neo4jGraphStore: generic get_neighbors_batch fallback");

        std::vector<NodeRef> copied(frontier.begin(), frontier.end());

        auto query = GetNeighborsBatchQuery(std::move(copied), spec);
        auto raw = visitor_->visit(query);

        if (!raw) {
            return {};
        }

        return parse_adjacency_rows(*raw, frontier);
    }

    BfsResult
    run_bfs(const NodeRef&,
            const ExpandSpec&,
            const BfsOptions&) override {
        return {};
    }

private:
    DatabaseContext& ctx_;
    BackendDescriptor descriptor_;
    std::shared_ptr<QueryVisitor> visitor_;

    NodeRef node_to_ref(const Node& node) const {
        /*
         * Normally, LocallyDeployedNeo4JResponseProcessor already maps Proxy
         * nodes to their logical remote type.
         *
         * This fallback is kept for older projected responses where remote-type
         * exists only as a property.
         */
        auto remote_type_it = node.properties.find("remote-type");

        if (remote_type_it != node.properties.end() &&
            remote_type_it->second &&
            remote_type_it->second->is<std::string>()) {
            return NodeRef{
                .global_id = node.global_id,
                .type = ctx_.schema->node_type_id(
                    remote_type_it->second->as<std::string>()
                )
            };
        }

        return NodeRef{
            .global_id = node.global_id,
            .type = node.type
        };
    }

    static EdgeRef edge_to_ref(const Edge& edge) {
        return EdgeRef{
            .global_id = edge.global_id,
            .type = edge.type,
            .from = edge.src,
            .to = edge.dst
        };
    }

    std::vector<AdjacencyRow>
    parse_adjacency_rows(const QueryResult& raw,
                         std::span<const NodeRef> frontier) const {
        if (frontier.empty()) {
            return {};
        }

        if (raw.data.empty()) {
            return {};
        }

        const auto& first_record = raw.data.front();

        if (!first_record || first_record->fields.empty()) {
            return {};
        }

        const std::size_t field_count = first_record->fields.size();

        if (field_count == 1) {
            return parse_neighbor_only_rows(raw, frontier);
        }

        return parse_source_neighbor_rows(raw);
    }

    std::vector<AdjacencyRow>
    parse_neighbor_only_rows(const QueryResult& raw,
                             std::span<const NodeRef> frontier) const {
        if (frontier.size() != 1) {
            throw std::runtime_error(
                "Cannot parse neighbor-only records for batched frontier. "
                "Each result row must include its source node."
            );
        }

        AdjacencyRow row;
        row.source = frontier.front();

        for (const auto& record_ptr : raw.data) {
            if (!record_ptr) {
                continue;
            }

            const Record& record = *record_ptr;

            if (record.fields.empty()) {
                continue;
            }

            const Value& value = record.fields[0];

            if (!value.is<Node>()) {
                throw std::runtime_error(
                    "Expected neighbor-only row field[0] to be Node"
                );
            }

            const Node& neighbor = value.as<Node>();

            row.neighbors.push_back(NeighborRef{
                .vertex = node_to_ref(neighbor),
                .via_edge = std::nullopt
            });
        }

        return {std::move(row)};
    }

    std::vector<AdjacencyRow>
    parse_source_neighbor_rows(const QueryResult& raw) const {
        std::vector<AdjacencyRow> rows;

        std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>>
            source_to_row;

        for (const auto& record_ptr : raw.data) {
            if (!record_ptr) {
                continue;
            }

            const Record& record = *record_ptr;

            if (record.fields.size() < 2) {
                throw std::runtime_error(
                    "Expected source-neighbor row to have at least 2 fields"
                );
            }

            if (!record.fields[0].is<Node>()) {
                throw std::runtime_error(
                    "Expected field[0] to be source Node"
                );
            }

            if (!record.fields[1].is<Node>()) {
                throw std::runtime_error(
                    "Expected field[1] to be neighbor Node"
                );
            }

            const Node& source = record.fields[0].as<Node>();
            const Node& neighbor = record.fields[1].as<Node>();

            NodeRef source_ref = node_to_ref(source);
            NodeRef neighbor_ref = node_to_ref(neighbor);

            std::optional<EdgeRef> edge_ref = std::nullopt;

            if (record.fields.size() >= 3) {
                const Value& edge_value = record.fields[2];

                if (!edge_value.is_null()) {
                    if (!edge_value.is<Edge>()) {
                        throw std::runtime_error(
                            "Expected field[2] to be Edge"
                        );
                    }

                    edge_ref = edge_to_ref(edge_value.as<Edge>());
                }
            }

            auto [it, inserted] = source_to_row.emplace(
                source_ref.global_id,
                rows.size()
            );

            if (inserted) {
                rows.push_back(AdjacencyRow{
                    .source = source_ref,
                    .neighbors = {}
                });
            }

            rows[it->second].neighbors.push_back(NeighborRef{
                .vertex = neighbor_ref,
                .via_edge = edge_ref
            });
        }

        return rows;
    }
};

#endif // CPGDB_NEO4JGRAPHSTORE_HPP