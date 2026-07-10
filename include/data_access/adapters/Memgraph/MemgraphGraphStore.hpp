//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_MEMGRAPHGRAPHSTORE_HPP
#define CPGDB_MEMGRAPHGRAPHSTORE_HPP
#include "algorithms/bfs/BfsResult.hpp"
#include "data_access/IGraphStore.hpp"

class MemgraphGraphStore final : public IGraphStore {
public:
    explicit MemgraphGraphStore(BackendDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}

    const BackendDescriptor& descriptor() const override {
        return descriptor_;
    }

    std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec, int depth) override {
        /*
         * Build a Cypher query similar to:
         *
         * UNWIND $frontier AS source_uuid
         * MATCH (source {uuid: source_uuid})-[r]-(neighbor)
         * WHERE size($edgeTypes) = 0 OR type(r) IN $edgeTypes
         * RETURN source.uuid, neighbor.uuid, labels(neighbor), type(r), properties(r)
         *
         * Then parse into AdjacencyRow.
         *
         * You can implement this using your existing QueryVisitor:
         *   visitor.visit(GetNeighborsBatchQuery{frontier, spec});
         */

        return execute_memgraph_batch_neighbors(frontier, spec);
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
        if (!can_run_native_bfs(source, spec, options)) {
            throw std::runtime_error("Memgraph cannot answer whole BFS");
        }

        /*
         * Example Memgraph-style idea:
         *
         * MATCH path=(s {uuid: $source})-[*BFS .. $maxDepth]-(n)
         * RETURN n.uuid AS uuid, length(path) AS distance
         *
         * For edge type restrictions:
         *
         * MATCH path=(s {uuid: $source})-[relationships:LIKES *BFS ..5]-(n)
         * RETURN n.uuid, length(path)
         *
         * Build the exact syntax based on your ExpandSpec.
         */

        return execute_memgraph_native_bfs(source, spec, options);
    }

private:
    std::vector<AdjacencyRow>
    execute_memgraph_batch_neighbors(std::span<const NodeRef> frontier,
                                     const ExpandSpec& spec) {
        // Adapter point:
        // call your Memgraph/Neo4j-compatible QueryVisitor here.
        throw std::runtime_error("Wire Memgraph batch query here");
    }

    BfsResult execute_memgraph_native_bfs(const NodeRef& source,
                                          const ExpandSpec& spec,
                                          const BfsOptions& options) {
        // Adapter point:
        // call your Memgraph QueryVisitor here and parse paths/distances.
        throw std::runtime_error("Wire Memgraph native BFS here");
    }

private:
    BackendDescriptor descriptor_;
};

#endif //CPGDB_MEMGRAPHGRAPHSTORE_HPP