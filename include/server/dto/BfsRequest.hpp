/**
 * @file include/server/dto/BfsRequest.hpp
 * @brief Declarations for bfs request in the server frontend module.
 */

#ifndef CPGDB_SERVER_DTO_BFS_REQUEST_HPP
#define CPGDB_SERVER_DTO_BFS_REQUEST_HPP

#include <optional>
#include <string>

namespace Json {
class Value;
}

namespace cpgdb::dto {

struct BfsRequest {
    std::string sourceVertexId;
    int maxDepth = 1;
    bool includeEdges = false;
    std::optional<std::string> relationshipFilter;

    [[nodiscard]] static BfsRequest fromJson(const Json::Value& json);
};

} // namespace cpgdb::dto

#endif // CPGDB_SERVER_DTO_BFS_REQUEST_HPP
