/**
 * @file src/cli/CurlGlobalGuard.cpp
 * @brief Implementation for curl global guard in the command-line and runtime configuration module.
 */

#include "cli/CurlGlobalGuard.hpp"

#include <curl/curl.h>
#include <stdexcept>

namespace polygraph::runtime {

    CurlGlobalGuard::CurlGlobalGuard() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("curl_global_init() failed");
        }
    }

    CurlGlobalGuard::~CurlGlobalGuard() { curl_global_cleanup(); }

} // namespace polygraph::runtime
