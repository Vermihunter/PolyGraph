//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_IGRAPHSTORE_HPP
#define CPGDB_IGRAPHSTORE_HPP
#include <span>
#include <unordered_map>
#include <vector>
#include <boost/container_hash/hash.hpp>

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "data_model/EntityId.hpp"
#include "data_model/NodeRef.hpp"
#include "infrastructure/BackendCapabilities.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "infrastructure/BackendKind.hpp"

class IGraphStore {
public:
    virtual ~IGraphStore() = default;

    virtual BackendKind kind() const = 0;
    virtual const BackendDescriptor& descriptor() const = 0;

    virtual std::vector<AdjacencyRow>
    get_neighbors_batch(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec, int depth) = 0;

    virtual std::vector<MinimalAdjacencyRow>
    get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                const ExpandSpec& spec, int depth) {
        auto rich_rows = get_neighbors_batch(frontier, spec, depth);

        std::vector<MinimalAdjacencyRow> minimal_rows;
        minimal_rows.reserve(rich_rows.size());

        for (auto& row : rich_rows) {
            MinimalAdjacencyRow minimal;
            minimal.source = row.source;
            minimal.neighbors.reserve(row.neighbors.size());

            for (const auto& neighbor : row.neighbors) {
                minimal.neighbors.push_back(neighbor.vertex);
            }

            minimal_rows.push_back(std::move(minimal));
        }

        return minimal_rows;
    }

    virtual bool can_run_native_bfs(const NodeRef& source,
                                    const ExpandSpec& spec,
                                    const BfsOptions& options) const {
        const auto d = descriptor();

        return options.allow_native_pushdown &&
               d.capabilities.supports_native_bfs &&
               d.coverage.can_answer_whole_bfs(source, spec);
    }

    virtual BfsResult
    run_bfs(const NodeRef& source, const ExpandSpec& spec, const BfsOptions& options) {
        throw std::runtime_error("Native BFS not supported");
    }
};

#endif //CPGDB_IGRAPHSTORE_HPP