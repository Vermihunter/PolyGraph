/**
 * @file include/cli/CurlGlobalGuard.hpp
 * @brief Declarations for curl global guard in the command-line and runtime configuration module.
 */

#ifndef CPGDB_CLI_CURL_GLOBAL_GUARD_HPP
#define CPGDB_CLI_CURL_GLOBAL_GUARD_HPP

namespace cpgdb::runtime {

class CurlGlobalGuard {
  public:
    CurlGlobalGuard();
    CurlGlobalGuard(const CurlGlobalGuard&) = delete;
    CurlGlobalGuard& operator=(const CurlGlobalGuard&) = delete;
    ~CurlGlobalGuard();
};

} // namespace cpgdb::runtime

#endif // CPGDB_CLI_CURL_GLOBAL_GUARD_HPP
