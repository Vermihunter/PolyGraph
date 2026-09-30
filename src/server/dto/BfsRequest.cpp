/**
 * @file src/server/dto/BfsRequest.cpp
 * @brief Implementation for BFS HTTP request parsing and validation.
 */

#include "server/dto/BfsRequest.hpp"

#include <json/value.h>
#include <limits>
#include <stdexcept>
#include <string>

namespace polygraph::dto {
    namespace {

        [[nodiscard]] std::size_t positive_size(const Json::Value &json, const char *key, const std::size_t fallback,
                                                const bool allow_zero = false) {
            if (!json.isMember(key)) {
                return fallback;
            }
            const auto &value = json[key];
            if (!value.isUInt64() && !value.isUInt()) {
                throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
            }
            const auto parsed = value.asUInt64();
            if ((!allow_zero && parsed == 0U) || parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(std::string(key) +
                                            (allow_zero ? " is too large" : " must be greater than zero"));
            }
            return static_cast<std::size_t>(parsed);
        }


        [[nodiscard]] int bounded_int(const Json::Value &json, const char *key, const int fallback, const int minimum,
                                      const int maximum) {
            if (!json.isMember(key)) {
                return fallback;
            }
            if (!json[key].isInt()) {
                throw std::invalid_argument(std::string(key) + " must be an integer");
            }
            const int parsed = json[key].asInt();
            if (parsed < minimum || parsed > maximum) {
                throw std::invalid_argument(std::string(key) + " is outside the supported range");
            }
            return parsed;
        }

        [[nodiscard]] std::vector<std::string> string_array(const Json::Value &json, const char *key) {
            if (!json.isMember(key)) {
                return {};
            }
            const auto &values = json[key];
            if (!values.isArray()) {
                throw std::invalid_argument(std::string(key) + " must be an array of strings");
            }
            std::vector<std::string> result;
            result.reserve(values.size());
            for (const auto &value: values) {
                if (!value.isString() || value.asString().empty()) {
                    throw std::invalid_argument(std::string(key) + " must contain only non-empty strings");
                }
                result.push_back(value.asString());
            }
            return result;
        }

        void optional_bool(const Json::Value &json, const char *key, bool &target) {
            if (!json.isMember(key)) {
                return;
            }
            if (!json[key].isBool()) {
                throw std::invalid_argument(std::string(key) + " must be a boolean");
            }
            target = json[key].asBool();
        }

    } // namespace

    BfsRequest BfsRequest::fromJson(const Json::Value &json) {
        BfsRequest request;
        if (json.isMember("sourceVertexId")) {
            if (!json["sourceVertexId"].isString()) {
                throw std::invalid_argument("sourceVertexId must be a string");
            }
            request.sourceVertexId = json["sourceVertexId"].asString();
        }

        if (json.isMember("maxDepth")) {
            if (!json["maxDepth"].isInt() || json["maxDepth"].asInt() < -1) {
                throw std::invalid_argument("maxDepth must be -1 or a non-negative integer");
            }
            request.maxDepth = json["maxDepth"].asInt();
        }

        optional_bool(json, "includeEdges", request.includeEdges);
        optional_bool(json, "returnPredecessors", request.returnPredecessors);
        optional_bool(json, "allowNativePushdown", request.allowNativePushdown);
        optional_bool(json, "allowPartialAcceleration", request.allowPartialAcceleration);

        if (json.isMember("relationshipFilter")) {
            if (!json["relationshipFilter"].isString()) {
                throw std::invalid_argument("relationshipFilter must be a string");
            }
            request.relationshipFilter = json["relationshipFilter"].asString();
        }

        if (json.isMember("direction")) {
            if (!json["direction"].isString()) {
                throw std::invalid_argument("direction must be a string");
            }
            request.direction = json["direction"].asString();
            if (request.direction != "directed" && request.direction != "undirected" && request.direction != "both") {
                throw std::invalid_argument("direction must be directed, undirected, or both");
            }
        }

        request.allowedEdgeTypes = string_array(json, "allowedEdgeTypes");
        request.allowedTargetNodeTypes = string_array(json, "allowedTargetNodeTypes");
        request.neighborBatchSize = positive_size(json, "neighborBatchSize", request.neighborBatchSize);
        request.neo4jMaxConcurrentRequests =
                positive_size(json, "neo4jMaxConcurrentRequests", request.neo4jMaxConcurrentRequests);
        request.arangoMaxConcurrentRequests =
                positive_size(json, "arangoMaxConcurrentRequests", request.arangoMaxConcurrentRequests);
        request.arangoTraversalBatchSize =
                positive_size(json, "arangoTraversalBatchSize", request.arangoTraversalBatchSize);
        if (json.isMember("arangoRequestStrategy")) {
            if (!json["arangoRequestStrategy"].isString()) {
                throw std::invalid_argument("arangoRequestStrategy must be a string");
            }
            request.arangoRequestStrategy = json["arangoRequestStrategy"].asString();
            if (request.arangoRequestStrategy != "chunked" && request.arangoRequestStrategy != "single" &&
                request.arangoRequestStrategy != "auto") {
                throw std::invalid_argument("arangoRequestStrategy must be chunked, single, or auto");
            }
        }
        if (json.isMember("arangoExpandMode")) {
            if (!json["arangoExpandMode"].isString()) {
                throw std::invalid_argument("arangoExpandMode must be a string");
            }
            request.arangoExpandMode = json["arangoExpandMode"].asString();
            if (request.arangoExpandMode != "traversal" && request.arangoExpandMode != "edge-index") {
                throw std::invalid_argument("arangoExpandMode must be traversal or edge-index");
            }
        }
        optional_bool(json, "arangoUsePlanCache", request.arangoUsePlanCache);
        request.arangoSingleRequestMaxFrontier =
                positive_size(json, "arangoSingleRequestMaxFrontier", request.arangoSingleRequestMaxFrontier);
        request.streamingCandidateBatchSize =
                positive_size(json, "streamingCandidateBatchSize", request.streamingCandidateBatchSize);
        request.redisPipelineBatchSize = positive_size(json, "redisPipelineBatchSize", request.redisPipelineBatchSize);
        request.queryExecutionId = positive_size(json, "queryExecutionId", request.queryExecutionId, true);
        request.backendSchedulerPriority =
                bounded_int(json, "backendSchedulerPriority", request.backendSchedulerPriority, -100, 100);
        request.backendSchedulerWeight = positive_size(json, "backendSchedulerWeight", request.backendSchedulerWeight);
        if (request.backendSchedulerWeight > 64) {
            throw std::invalid_argument("backendSchedulerWeight must be between 1 and 64");
        }
        request.backendRequestRetries =
                positive_size(json, "backendRequestRetries", request.backendRequestRetries, true);
        request.backendRetryDelayMs = positive_size(json, "backendRetryDelayMs", request.backendRetryDelayMs, true);
        request.threadCount = positive_size(json, "threadCount", request.threadCount);

        if (json.isMember("executionPolicy")) {
            if (!json["executionPolicy"].isString()) {
                throw std::invalid_argument("executionPolicy must be a string");
            }
            request.executionPolicy = json["executionPolicy"].asString();
            if (request.executionPolicy != "classic" && request.executionPolicy != "direction-optimized") {
                throw std::invalid_argument("executionPolicy must be classic or direction-optimized");
            }
        }
        request.pullEnterFrontierPercent =
                positive_size(json, "pullEnterFrontierPercent", request.pullEnterFrontierPercent);
        request.pullExitFrontierPercent =
                positive_size(json, "pullExitFrontierPercent", request.pullExitFrontierPercent, true);
        request.bitmapFrontierMinSize = positive_size(json, "bitmapFrontierMinSize", request.bitmapFrontierMinSize);
        request.bitmapFrontierDensityPercent =
                positive_size(json, "bitmapFrontierDensityPercent", request.bitmapFrontierDensityPercent);
        if (request.pullEnterFrontierPercent > 100 || request.pullExitFrontierPercent > 100 ||
            request.pullExitFrontierPercent > request.pullEnterFrontierPercent) {
            throw std::invalid_argument("pull frontier percentages must satisfy 0 <= exit <= enter <= 100");
        }
        if (request.bitmapFrontierDensityPercent > 100) {
            throw std::invalid_argument("bitmapFrontierDensityPercent must be between 1 and 100");
        }

        if (json.isMember("responseMode")) {
            if (!json["responseMode"].isString()) {
                throw std::invalid_argument("responseMode must be a string");
            }
            request.responseMode = json["responseMode"].asString();
            if (request.responseMode != "full" && request.responseMode != "summary") {
                throw std::invalid_argument("responseMode must be full or summary");
            }
        }

        return request;
    }

} // namespace polygraph::dto
