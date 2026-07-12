/**
 * @file src/serialization/BfsResultJsonSerializer.cpp
 * @brief Implementation for bfs result json serializer in the serialization module.
 */

#include "serialization/BfsResultJsonSerializer.hpp"

#include <boost/uuid/uuid_io.hpp>
#include <utility>

namespace cpgdb {

    namespace {
        std::string globalIdToString(const GlobalId &id) { return boost::uuids::to_string(id); }
    } // namespace

    void to_json(nlohmann::json &json, const BfsResult &result) {
        auto distances = nlohmann::json::object();
        for (const auto &[vertex_id, distance]: result.distance) {
            distances[globalIdToString(vertex_id)] = distance;
        }

        auto predecessors = nlohmann::json::object();
        for (const auto &[vertex_id, predecessor_id]: result.predecessor) {
            predecessors[globalIdToString(vertex_id)] = globalIdToString(predecessor_id);
        }

        json = {{"distance", std::move(distances)}, {"predecessor", std::move(predecessors)}};
    }

} // namespace cpgdb
