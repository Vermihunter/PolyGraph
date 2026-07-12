/**
 * @file src/server/http/JsonResponse.cpp
 * @brief Implementation for json response in the server frontend module.
 */

#include "server/http/JsonResponse.hpp"

#include <utility>

namespace cpgdb::http {

    drogon::HttpResponsePtr jsonResponse(drogon::HttpStatusCode status, const nlohmann::json &body) {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(status);
        response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        response->setBody(body.dump());
        return response;
    }

    drogon::HttpResponsePtr jsonOk(const nlohmann::json &body) { return jsonResponse(drogon::k200OK, body); }

} // namespace cpgdb::http
