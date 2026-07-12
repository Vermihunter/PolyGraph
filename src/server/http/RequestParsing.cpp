/**
 * @file src/server/http/RequestParsing.cpp
 * @brief Implementation for request parsing in the server frontend module.
 */

#include "server/http/RequestParsing.hpp"

#include "server/http/ErrorMapper.hpp"

namespace cpgdb::http::parse {

    const Json::Value &jsonBody(const drogon::HttpRequestPtr &request) {
        const auto &json = request->getJsonObject();
        if (!json) {
            throw BadRequest("Expected request body with Content-Type: application/json");
        }
        if (!json->isObject()) {
            throw BadRequest("Expected JSON object body");
        }
        return *json;
    }

} // namespace cpgdb::http::parse
