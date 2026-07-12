/**
 * @file src/server/dto/BfsRequest.cpp
 * @brief Implementation for bfs request in the server frontend module.
 */

#include "server/dto/BfsRequest.hpp"

#include <json/value.h>
#include <stdexcept>

namespace cpgdb::dto {

    BfsRequest BfsRequest::fromJson(const Json::Value &json) {
        /*
        if (!json.isMember("sourceVertexId") || !json["sourceVertexId"].isString()) {
            throw std::invalid_argument("sourceVertexId is required and must be a string");
        }
        */

        BfsRequest request;
        request.sourceVertexId = json["sourceVertexId"].asString();

        if (json.isMember("maxDepth")) {
            if (!json["maxDepth"].isInt() || json["maxDepth"].asInt() < 0) {
                throw std::invalid_argument("maxDepth must be a non-negative integer");
            }
            request.maxDepth = json["maxDepth"].asInt();
        }

        if (json.isMember("includeEdges")) {
            if (!json["includeEdges"].isBool()) {
                throw std::invalid_argument("includeEdges must be a boolean");
            }
            request.includeEdges = json["includeEdges"].asBool();
        }

        if (json.isMember("relationshipFilter")) {
            if (!json["relationshipFilter"].isString()) {
                throw std::invalid_argument("relationshipFilter must be a string");
            }
            request.relationshipFilter = json["relationshipFilter"].asString();
        }

        return request;
    }

} // namespace cpgdb::dto
