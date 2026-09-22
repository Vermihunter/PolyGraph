/**
 * @file include/server/http/JsonResponse.hpp
 * @brief Declarations for json response in the server frontend module.
 */

#pragma once

#include "utilities/json.hpp"

#include <drogon/HttpResponse.h>

namespace polygraph::http {

[[nodiscard]] drogon::HttpResponsePtr jsonResponse(drogon::HttpStatusCode status,
                                                   const nlohmann::json& body);

[[nodiscard]] drogon::HttpResponsePtr jsonOk(const nlohmann::json& body);

} // namespace polygraph::http
