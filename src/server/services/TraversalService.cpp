/**
 * @file src/server/services/TraversalService.cpp
 * @brief Implementation for traversal service in the server frontend module.
 */

#include "server/services/TraversalService.hpp"
#include "utilities/uuid.hpp"

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsExecutor.hpp"
#include "algorithms/bfs/BfsOptions.hpp"


namespace cpgdb::services {

    TraversalService::TraversalService(DatabaseContext &context) noexcept :
        context_(context), expander_(*context.placement_catalog) {}

    BfsResult TraversalService::runBfs(const std::string &source_vertex_uuid, const dto::BfsRequest &request) {
        BfsExecutor bfs(*context_.placement_catalog);
        const NodeRef source = context_.node_ref(global_id_from_string(source_vertex_uuid));
        const BfsOptions options{.max_depth = static_cast<std::uint32_t>(request.maxDepth),
                                 .return_predecessors = true,
                                 .allow_native_pushdown = false,
                                 .allow_partial_acceleration = true};
        return bfs.run(source, ExpandSpec{}, options);
    }

    std::vector<MinimalAdjacencyRow> TraversalService::getNeighbors(const std::string &source_vertex_uuid) {
        const NodeRef source = context_.node_ref(global_id_from_string(source_vertex_uuid));
        std::vector frontier{source};

        const ExpandSpec spec{};
        const BfsOptions options{.max_depth = 1,
                                 .return_predecessors = false,
                                 .allow_native_pushdown = false,
                                 .allow_partial_acceleration = true};

        return expander_.expand_layer(frontier, spec, 0, options);
    }

    BfsResult TraversalService::streamBfs(const std::string &graph_id, const std::string &source_vertex_id,
                                          std::uint32_t max_depth, bool return_predecessors, bool allow_native_pushdown,
                                          bool allow_partial_acceleration,
                                          const std::function<void(const BfsLayerEvent &)> &on_layer) {
        (void) graph_id;
        BfsExecutor bfs(*context_.placement_catalog);
        const NodeRef source = context_.node_ref(global_id_from_string(source_vertex_id));
        return bfs.run_streaming(source, ExpandSpec{},
                                 BfsOptions{.max_depth = max_depth,
                                            .return_predecessors = return_predecessors,
                                            .allow_native_pushdown = allow_native_pushdown,
                                            .allow_partial_acceleration = allow_partial_acceleration},
                                 on_layer);
    }

} // namespace cpgdb::services
