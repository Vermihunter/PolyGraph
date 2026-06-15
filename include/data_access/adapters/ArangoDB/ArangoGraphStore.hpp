#ifndef CPGDB_ARANGOGRAPHSTORE_HPP
#define CPGDB_ARANGOGRAPHSTORE_HPP

#include <boost/functional/hash.hpp>

#include "algorithms/bfs/BfsResult.hpp"
#include "data_access/IGraphStore.hpp"
#include "query/QueryVisitor.hpp"
#include "query/query_types/GetNeighborsBatchQuery.hpp"

class ArangoGraphStore final : public IGraphStore {
public:
    ArangoGraphStore(DatabaseContext& ctx,
                     BackendDescriptor descriptor,
                     std::shared_ptr<QueryVisitor> visitor)
        : ctx_(ctx),
          descriptor_(std::move(descriptor)),
          visitor_(std::move(visitor)) {}

    BackendKind kind() const override {
        return BackendKind::Arango;
    }

    const BackendDescriptor& descriptor() const override {
        return descriptor_;
    }

    std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec) override {
        std::vector<NodeRef> copied(frontier.begin(), frontier.end());

        auto query = GetNeighborsBatchQuery(std::move(copied), spec);
        auto raw = visitor_->visit(query);

        if (!raw) {
            return {};
        }

        return parse_adjacency_rows(*raw);
    }

    bool can_run_native_bfs(const NodeRef& source,
                            const ExpandSpec& spec,
                            const BfsOptions& options) const override {
        return options.allow_native_pushdown &&
               descriptor_.capabilities.supports_native_bfs &&
               descriptor_.coverage.can_answer_whole_bfs(source, spec);
    }

    BfsResult run_bfs(const NodeRef& source,
                      const ExpandSpec& spec,
                      const BfsOptions& options) override {
        throw std::runtime_error("Arango native BFS not wired yet");
    }

private:
    static NodeRef node_to_ref(const Node& node) {
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
    parse_adjacency_rows(const QueryResult& raw) const {
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
                    "Arango adjacency row expected at least source and neighbor"
                );
            }

            if (!record.fields[0].is<Node>()) {
                throw std::runtime_error(
                    "Arango adjacency row field[0] must be source Node"
                );
            }

            if (!record.fields[1].is<Node>()) {
                throw std::runtime_error(
                    "Arango adjacency row field[1] must be neighbor Node"
                );
            }

            const Node& source_node = record.fields[0].as<Node>();
            const Node& neighbor_node = record.fields[1].as<Node>();

            NodeRef source_ref = node_to_ref(source_node);
            NodeRef neighbor_ref = node_to_ref(neighbor_node);

            std::optional<EdgeRef> edge_ref = std::nullopt;

            if (record.fields.size() >= 3 && !record.fields[2].is_null()) {
                if (!record.fields[2].is<Edge>()) {
                    throw std::runtime_error(
                        "Arango adjacency row field[2] must be Edge"
                    );
                }

                edge_ref = edge_to_ref(record.fields[2].as<Edge>());
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
                .via_edge = std::move(edge_ref)
            });
        }

        return rows;
    }

private:
    DatabaseContext& ctx_;
    BackendDescriptor descriptor_;
    std::shared_ptr<QueryVisitor> visitor_;
};

#endif // CPGDB_ARANGOGRAPHSTORE_HPP