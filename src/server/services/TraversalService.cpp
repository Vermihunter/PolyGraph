//
// Created by akos on 7. 7. 2026.
//

#include "server/services/TraversalService.hpp"

#include <boost/lexical_cast.hpp>

#include "server/services/ServicesPlugin.hpp"

BfsResult services::TraversalService::runBfs(
            const std::string& graphId,
            const dto::BfsRequest& request
        ) {


    BfsExecutor bfs(*ctx_.placement_catalog);

    const NodeRef source {
        .global_id =  boost::lexical_cast<boost::uuids::uuid>(request.sourceVertexId),
        .type = ctx_.schema->node_type_id("Faculty")
    };
    const ExpandSpec expand_spec{

    };
    const BfsOptions options {
        .max_depth = request.maxDepth,
        .return_predecessors = true,
        .allow_native_pushdown = false,
        .allow_partial_acceleration = true
    };

    return bfs.run(source, expand_spec, options);
}

BfsResult services::TraversalService::streamBfs(
    const std::string& graphId,
    const std::string& sourceVertexId,
    const std::string& sourceType,
    std::uint32_t maxDepth,
    bool returnPredecessors,
    bool allowNativePushdown,
    bool allowPartialAcceleration,
    const std::function<void(const BfsLayerEvent&)>& onLayer
) {
    (void) graphId;

    BfsExecutor bfs(*ctx_.placement_catalog);

    const NodeRef source {
        .global_id = boost::lexical_cast<boost::uuids::uuid>(sourceVertexId),
        .type = ctx_.schema->node_type_id(sourceType.empty() ? "Faculty" : sourceType)
    };

    const ExpandSpec expand_spec{};

    BfsOptions options {
        .max_depth = maxDepth,
        .return_predecessors = returnPredecessors,
        .allow_native_pushdown = allowNativePushdown,
        .allow_partial_acceleration = allowPartialAcceleration
    };

    return bfs.run_streaming(source, expand_spec, options, onLayer);
}
