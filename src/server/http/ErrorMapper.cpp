/**
 * @file src/server/http/ErrorMapper.cpp
 * @brief Implementation for error mapper in the server frontend module.
 */

#include "server/http/ErrorMapper.hpp"

#include "server/http/JsonResponse.hpp"

#include <utility>

namespace polygraph::http {
    namespace {

        nlohmann::json make_error_body(const ErrorCode code, const std::string_view message) {
            return {{"error", {{"code", toString(code)}, {"message", std::string(message)}}}};
        }

        drogon::HttpResponsePtr json_error(const drogon::HttpStatusCode status, const ErrorCode code,
                                           const std::string_view message) {
            return jsonResponse(status, make_error_body(code, message));
        }

    } // namespace

    std::string toString(const ErrorCode code) {
        switch (code) {
            case ErrorCode::BadRequest:
                return "bad_request";
            case ErrorCode::ValidationError:
                return "validation_error";
            case ErrorCode::NotFound:
                return "not_found";
            case ErrorCode::InternalError:
                return "internal_error";
        }
        return "unknown_error";
    }

    HttpError::HttpError(const drogon::HttpStatusCode status, const ErrorCode code, const std::string &message) :
        std::runtime_error(message), status_(status), code_(code) {}

    drogon::HttpStatusCode HttpError::status() const noexcept { return status_; }

    ErrorCode HttpError::code() const noexcept { return code_; }

    BadRequest::BadRequest(const std::string &message) :
        HttpError(drogon::k400BadRequest, ErrorCode::BadRequest, message) {}

    drogon::HttpResponsePtr mapExceptionToResponse(const std::exception &exception) {
        if (const auto *error = dynamic_cast<const HttpError *>(&exception)) {
            return json_error(error->status(), error->code(), error->what());
        }
        if (dynamic_cast<const std::invalid_argument *>(&exception) != nullptr ||
            dynamic_cast<const std::runtime_error *>(&exception) != nullptr) {
            return json_error(drogon::k400BadRequest, ErrorCode::ValidationError, exception.what());
        }
        if (dynamic_cast<const std::out_of_range *>(&exception) != nullptr) {
            return json_error(drogon::k404NotFound, ErrorCode::NotFound, exception.what());
        }

        return json_error(drogon::k500InternalServerError, ErrorCode::InternalError,
                          std::string("Internal server error - ") + exception.what());
    }

    drogon::HttpResponsePtr mapUnknownExceptionToResponse() {
        return json_error(drogon::k500InternalServerError, ErrorCode::InternalError, "Unknown internal server error");
    }

} // namespace polygraph::http
