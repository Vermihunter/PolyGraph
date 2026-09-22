/**
 * @file include/server/http/RequestParsing.hpp
 * @brief Declarations for request parsing in the server frontend module.
 */

#pragma once

#include <drogon/HttpRequest.h>
#include <json/value.h>

namespace polygraph::http::parse {

[[nodiscard]] const Json::Value& jsonBody(const drogon::HttpRequestPtr& request);

} // namespace polygraph::http::parse
