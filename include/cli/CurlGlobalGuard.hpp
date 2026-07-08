//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_CURLGLOBALGUARD_HPP
#define CPGDB_CURLGLOBALGUARD_HPP

#include <curl/curl.h>

namespace cpgdb::runtime {

    class CurlGlobalGuard final {
    public:
        CurlGlobalGuard() {
            curl_global_init(CURL_GLOBAL_ALL);
        }

        ~CurlGlobalGuard() {
            curl_global_cleanup();
        }

        CurlGlobalGuard(const CurlGlobalGuard&) = delete;
        CurlGlobalGuard& operator=(const CurlGlobalGuard&) = delete;
    };

} // namespace cpgdb::runtime


#endif //CPGDB_CURLGLOBALGUARD_HPP
