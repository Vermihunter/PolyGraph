//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_ERRORMAPPER_HPP
#define CPGDB_ERRORMAPPER_HPP

#pragma once

#include "JsonResponse.hpp"

#include <drogon/HttpTypes.h>

#include <nlohmann/json.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace http {

enum class ErrorCode {
    BadRequest,
    Unauthorized,
    Forbidden,
    NotFound,
    Conflict,
    ValidationError,
    BackendUnavailable,
    InternalError
};

inline std::string toString(ErrorCode code) {
    switch (code) {
        case ErrorCode::BadRequest:
            return "bad_request";

        case ErrorCode::Unauthorized:
            return "unauthorized";

        case ErrorCode::Forbidden:
            return "forbidden";

        case ErrorCode::NotFound:
            return "not_found";

        case ErrorCode::Conflict:
            return "conflict";

        case ErrorCode::ValidationError:
            return "validation_error";

        case ErrorCode::BackendUnavailable:
            return "backend_unavailable";

        case ErrorCode::InternalError:
            return "internal_error";
    }

    return "unknown_error";
}

class HttpError : public std::runtime_error {
public:
    HttpError(
        drogon::HttpStatusCode status,
        ErrorCode code,
        std::string message
    )
        : std::runtime_error(std::move(message)),
          status_(status),
          code_(code) {}

    [[nodiscard]] drogon::HttpStatusCode status() const noexcept {
        return status_;
    }

    [[nodiscard]] ErrorCode code() const noexcept {
        return code_;
    }

private:
    drogon::HttpStatusCode status_;
    ErrorCode code_;
};

class BadRequest final : public HttpError {
public:
    explicit BadRequest(std::string message)
        : HttpError(
              drogon::k400BadRequest,
              ErrorCode::BadRequest,
              std::move(message)
          ) {}
};

class Unauthorized final : public HttpError {
public:
    explicit Unauthorized(std::string message = "Unauthorized")
        : HttpError(
              drogon::k401Unauthorized,
              ErrorCode::Unauthorized,
              std::move(message)
          ) {}
};

class Forbidden final : public HttpError {
public:
    explicit Forbidden(std::string message = "Forbidden")
        : HttpError(
              drogon::k403Forbidden,
              ErrorCode::Forbidden,
              std::move(message)
          ) {}
};

class NotFound final : public HttpError {
public:
    explicit NotFound(std::string message)
        : HttpError(
              drogon::k404NotFound,
              ErrorCode::NotFound,
              std::move(message)
          ) {}
};

class Conflict final : public HttpError {
public:
    explicit Conflict(std::string message)
        : HttpError(
              drogon::k409Conflict,
              ErrorCode::Conflict,
              std::move(message)
          ) {}
};

class BackendUnavailable final : public HttpError {
public:
    explicit BackendUnavailable(std::string message)
        : HttpError(
              drogon::k503ServiceUnavailable,
              ErrorCode::BackendUnavailable,
              std::move(message)
          ) {}
};

inline nlohmann::json makeErrorBody(
    ErrorCode code,
    std::string_view message
) {
    return {
        {
            "error",
            {
                {"code", toString(code)},
                {"message", std::string(message)}
            }
        }
    };
}

inline nlohmann::json makeErrorBody(
    ErrorCode code,
    std::string_view message,
    const nlohmann::json& details
) {
    return {
        {
            "error",
            {
                {"code", toString(code)},
                {"message", std::string(message)},
                {"details", details}
            }
        }
    };
}

inline drogon::HttpResponsePtr jsonError(
    drogon::HttpStatusCode status,
    ErrorCode code,
    std::string_view message
) {
    return jsonResponse(
        status,
        makeErrorBody(code, message)
    );
}

inline drogon::HttpResponsePtr jsonError(
    drogon::HttpStatusCode status,
    ErrorCode code,
    std::string_view message,
    const nlohmann::json& details
) {
    return jsonResponse(
        status,
        makeErrorBody(code, message, details)
    );
}

inline drogon::HttpResponsePtr mapExceptionToResponse(
    const std::exception& exception
) {
    if (const auto* error = dynamic_cast<const HttpError*>(&exception)) {
        return jsonError(
            error->status(),
            error->code(),
            error->what()
        );
    }

    if (dynamic_cast<const std::invalid_argument*>(&exception) != nullptr) {
        return jsonError(
            drogon::k400BadRequest,
            ErrorCode::ValidationError,
            exception.what()
        );
    }

    if (dynamic_cast<const std::out_of_range*>(&exception) != nullptr) {
        return jsonError(
            drogon::k404NotFound,
            ErrorCode::NotFound,
            exception.what()
        );
    }

    return jsonError(
        drogon::k500InternalServerError,
        ErrorCode::InternalError,
        "Internal server error"
    );
}

inline drogon::HttpResponsePtr mapUnknownExceptionToResponse() {
    return jsonError(
        drogon::k500InternalServerError,
        ErrorCode::InternalError,
        "Unknown internal server error"
    );
}

} // namespace http

#endif //CPGDB_ERRORMAPPER_HPP
