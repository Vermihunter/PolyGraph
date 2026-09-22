/**
 * @file include/cli/CurlGlobalGuard.hpp
 * @brief Declarations for curl global guard in the command-line and runtime configuration module.
 */

#ifndef POLYGRAPH_CLI_CURL_GLOBAL_GUARD_HPP
#define POLYGRAPH_CLI_CURL_GLOBAL_GUARD_HPP

namespace polygraph::runtime {

class CurlGlobalGuard {
  public:
    CurlGlobalGuard();
    CurlGlobalGuard(const CurlGlobalGuard&) = delete;
    CurlGlobalGuard& operator=(const CurlGlobalGuard&) = delete;
    ~CurlGlobalGuard();
};

} // namespace polygraph::runtime

#endif // POLYGRAPH_CLI_CURL_GLOBAL_GUARD_HPP
