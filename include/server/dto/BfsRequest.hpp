//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_BFSREQUEST_HPP
#define CPGDB_BFSREQUEST_HPP

#include <json/value.h>
#include <string>
#include <optional>

namespace dto {

    struct BfsRequest {
        std::string sourceVertexId;
        int maxDepth = 1;
        bool includeEdges = false;
        std::optional<std::string> relationshipFilter;

        static BfsRequest fromJson(const Json::Value& json) {
            BfsRequest req;

            if (!json.isMember("sourceVertexId") || !json["sourceVertexId"].isString()) {
                throw std::invalid_argument("sourceVertexId is required");
            }

            req.sourceVertexId = json["sourceVertexId"].asString();

            if (json.isMember("maxDepth")) {
                req.maxDepth = json["maxDepth"].asInt();
            }

            if (json.isMember("includeEdges")) {
                req.includeEdges = json["includeEdges"].asBool();
            }

            if (json.isMember("relationshipFilter")) {
                req.relationshipFilter = json["relationshipFilter"].asString();
            }

            return req;
        }
    };

}

#endif //CPGDB_BFSREQUEST_HPP
