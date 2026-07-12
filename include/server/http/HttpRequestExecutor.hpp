//
// Created by akos on 12. 7. 2026.
//

#ifndef CPGDB_HTTPREQUESTEXECUTOR_HPP
#define CPGDB_HTTPREQUESTEXECUTOR_HPP
#include "ErrorMapper.hpp"

#include <drogon/HttpResponse.h>
#include <functional>

namespace cpgdb::http {

class HttpRequestExecutor {
  public:
    using ResponseCallback = std::function<void(const drogon::HttpResponsePtr&)>;

    template <typename Handler>
    static void executeRequest(ResponseCallback&& callback, Handler&& handler) {
        auto response_callback = std::move(callback);

        try {
            response_callback(std::forward<Handler>(handler)());
        } catch (const std::exception& error) {
            response_callback(mapExceptionToResponse(error));
        } catch (...) {
            response_callback(mapUnknownExceptionToResponse());
        }
    }
};

} // namespace cpgdb::http

#endif // CPGDB_HTTPREQUESTEXECUTOR_HPP
