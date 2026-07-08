//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_BFSRESULTJSONSERIALIZER_HPP
#define CPGDB_BFSRESULTJSONSERIALIZER_HPP
#include <boost/uuid/uuid_io.hpp>

#include "algorithms/bfs/BfsResult.hpp"
#include "utilities/json.hpp"

inline std::string globalIdToString(const GlobalId& id) {
    return boost::uuids::to_string(id);
}

inline void to_json(nlohmann::json& j, const BfsResult& result) {
    nlohmann::json distanceJson = nlohmann::json::object();

    for (const auto& [vertexId, distance] : result.distance) {
        distanceJson[globalIdToString(vertexId)] = distance;
    }

    nlohmann::json predecessorJson = nlohmann::json::object();

    for (const auto& [vertexId, predecessorId] : result.predecessor) {
        predecessorJson[globalIdToString(vertexId)] =
            globalIdToString(predecessorId);
    }

    j = nlohmann::json{
            {"distance", std::move(distanceJson)},
            {"predecessor", std::move(predecessorJson)}
    };
}

#endif //CPGDB_BFSRESULTJSONSERIALIZER_HPP
