/**
 * @file include/server/dto/BfsRequest.hpp
 * @brief HTTP BFS request options mirroring the native BFS CLI.
 */

#ifndef POLYGRAPH_SERVER_DTO_BFS_REQUEST_HPP
#define POLYGRAPH_SERVER_DTO_BFS_REQUEST_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Json {
class Value;
}

namespace polygraph::dto {

/**
 * @brief Request-time BFS options accepted by the HTTP traversal endpoint.
 *
 * The defaults preserve the historical HTTP endpoint behaviour.  Benchmarking
 * clients can override the same knobs exposed by the native BFS CLI without
 * rebuilding or restarting the runtime between repetitions.
 */
struct BfsRequest {
    std::string sourceVertexId;
    int maxDepth = 1;
    bool includeEdges = false;
    std::optional<std::string> relationshipFilter;

    std::string direction = "both";
    std::vector<std::string> allowedEdgeTypes;
    std::vector<std::string> allowedTargetNodeTypes;
    bool returnPredecessors = true;
    bool allowNativePushdown = false;
    bool allowPartialAcceleration = true;
    std::size_t neighborBatchSize = 10'000;
    std::size_t neo4jMaxConcurrentRequests = 4;
    std::size_t arangoMaxConcurrentRequests = 4;
    std::size_t arangoTraversalBatchSize = 1'024;
    std::string arangoRequestStrategy = "chunked";
    std::string arangoExpandMode = "edge-index";
    bool arangoUsePlanCache = true;
    std::size_t arangoSingleRequestMaxFrontier = 50'000;
    std::size_t streamingCandidateBatchSize = 16'384;
    std::size_t redisPipelineBatchSize = 20'000;
    // Stable correlation ID supplied by the HTTP benchmark. Zero keeps normal auto-assignment.
    std::size_t queryExecutionId = 0;
    int backendSchedulerPriority = 0;
    std::size_t backendSchedulerWeight = 1;
    std::size_t backendRequestRetries = 2;
    std::size_t backendRetryDelayMs = 100;
    std::size_t threadCount = 1;

    std::string executionPolicy = "classic";
    std::size_t pullEnterFrontierPercent = 5;
    std::size_t pullExitFrontierPercent = 1;
    std::size_t bitmapFrontierMinSize = 4'096;
    std::size_t bitmapFrontierDensityPercent = 10;
    std::string responseMode = "full";

    [[nodiscard]] static BfsRequest fromJson(const Json::Value& json);
};

} // namespace polygraph::dto

#endif // POLYGRAPH_SERVER_DTO_BFS_REQUEST_HPP
