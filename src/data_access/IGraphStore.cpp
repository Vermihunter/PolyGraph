/**
 * @file src/data_access/IGraphStore.cpp
 * @brief Implementation for igraph store in the graph-store access module.
 */

#include "data_access/IGraphStore.hpp"

#include "query/Query.hpp"

#include <stdexcept>
#include <utility>

namespace cpgdb {

    IGraphStore::~IGraphStore() = default;

    query_result_t IGraphStore::execute(const Query &) {
        throw std::logic_error("This graph store does not support general query execution");
    }

    std::vector<MinimalAdjacencyRow> IGraphStore::get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                              const ExpandSpec &spec, int depth,
                                                                              const BfsOptions &options) {
        (void) options;
        auto rich_rows = get_neighbors_batch(frontier, spec, depth);
        std::vector<MinimalAdjacencyRow> minimal_rows;
        minimal_rows.reserve(rich_rows.size());

        for (AdjacencyRow &row: rich_rows) {
            MinimalAdjacencyRow minimal{.source = row.source, .neighbors = {}};
            minimal.neighbors.reserve(row.neighbors.size());
            for (const NeighborRef &neighbor: row.neighbors) {
                minimal.neighbors.push_back(neighbor.vertex);
            }
            minimal_rows.push_back(std::move(minimal));
        }

        return minimal_rows;
    }

    bool IGraphStore::can_run_native_bfs(const NodeRef &source, const ExpandSpec &spec,
                                         const BfsOptions &options) const {
        const BackendDescriptor &value = descriptor();
        return options.allow_native_pushdown && value.capabilities.supports_native_bfs &&
               value.coverage.can_answer_whole_bfs(source, spec);
    }

    BfsResult IGraphStore::run_bfs(const NodeRef &, const ExpandSpec &, const BfsOptions &) {
        throw std::runtime_error("Native BFS not supported by this graph store");
    }

} // namespace cpgdb
