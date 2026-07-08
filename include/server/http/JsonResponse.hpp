//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_JSONRESPONSE_HPP
#define CPGDB_JSONRESPONSE_HPP

#pragma once

#include <drogon/HttpResponse.h>

#include <json/value.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace http {

inline drogon::HttpResponsePtr jsonResponse(
    drogon::HttpStatusCode status,
    const Json::Value& body
) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    return response;
}

inline drogon::HttpResponsePtr jsonResponse(
    drogon::HttpStatusCode status,
    Json::Value&& body
) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    return response;
}

inline drogon::HttpResponsePtr jsonResponse(
    drogon::HttpStatusCode status,
    const nlohmann::json& body
) {
    auto response = drogon::HttpResponse::newHttpResponse();

    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(body.dump());

    return response;
}

inline drogon::HttpResponsePtr jsonResponse(
    drogon::HttpStatusCode status,
    nlohmann::json&& body
) {
    auto response = drogon::HttpResponse::newHttpResponse();

    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(body.dump());

    return response;
}

inline drogon::HttpResponsePtr jsonOk(const Json::Value& body) {
    return jsonResponse(drogon::k200OK, body);
}

inline drogon::HttpResponsePtr jsonOk(Json::Value&& body) {
    return jsonResponse(drogon::k200OK, std::move(body));
}

inline drogon::HttpResponsePtr jsonOk(const nlohmann::json& body) {
    return jsonResponse(drogon::k200OK, body);
}

inline drogon::HttpResponsePtr jsonOk(nlohmann::json&& body) {
    return jsonResponse(drogon::k200OK, std::move(body));
}

inline drogon::HttpResponsePtr jsonCreated(const Json::Value& body) {
    return jsonResponse(drogon::k201Created, body);
}

inline drogon::HttpResponsePtr jsonCreated(Json::Value&& body) {
    return jsonResponse(drogon::k201Created, std::move(body));
}

inline drogon::HttpResponsePtr jsonCreated(const nlohmann::json& body) {
    return jsonResponse(drogon::k201Created, body);
}

inline drogon::HttpResponsePtr jsonCreated(nlohmann::json&& body) {
    return jsonResponse(drogon::k201Created, std::move(body));
}

inline drogon::HttpResponsePtr jsonNoContent() {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    return response;
}

inline drogon::HttpResponsePtr plainText(
    drogon::HttpStatusCode status,
    std::string body
) {
    auto response = drogon::HttpResponse::newHttpResponse();

    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    response->setBody(std::move(body));

    return response;
}

} // namespace http

#endif //CPGDB_JSONRESPONSE_HPP
