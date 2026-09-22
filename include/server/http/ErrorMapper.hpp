/**
 * @file include/server/http/ErrorMapper.hpp
 * @brief Declarations for error mapper in the server frontend module.
 */

#pragma once

#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace polygraph::http {

enum class ErrorCode { BadRequest, ValidationError, NotFound, InternalError };

[[nodiscard]] std::string toString(ErrorCode code);

class HttpError : public std::runtime_error {
  public:
    HttpError(drogon::HttpStatusCode status, ErrorCode code, const std::string& message);

    [[nodiscard]] drogon::HttpStatusCode status() const noexcept;
    [[nodiscard]] ErrorCode code() const noexcept;

  private:
    drogon::HttpStatusCode status_;
    ErrorCode code_;
};

class BadRequest final : public HttpError {
  public:
    explicit BadRequest(const std::string& message);
};

[[nodiscard]] drogon::HttpResponsePtr mapExceptionToResponse(const std::exception& exception);
[[nodiscard]] drogon::HttpResponsePtr mapUnknownExceptionToResponse();

} // namespace polygraph::http
