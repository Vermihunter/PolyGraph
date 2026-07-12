/**
 * @file include/utilities/curl.hpp
 * @brief Declarations for curl in the utility module.
 */

#ifndef CPGDB_UTILITIES_CURL_HPP
#define CPGDB_UTILITIES_CURL_HPP

#include <curl/curl.h>
#include <string>

namespace cpgdb {

class CurlHttpClient {
  public:
    explicit CurlHttpClient(std::string auth = {});
    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;
    CurlHttpClient(CurlHttpClient&& other) noexcept;
    CurlHttpClient& operator=(CurlHttpClient&& other) noexcept;
    ~CurlHttpClient();

    [[nodiscard]] std::string post(const std::string& url, const std::string& body);
    [[nodiscard]] std::string put(const std::string& url, const std::string& body = "{}");
    [[nodiscard]] std::string get(const std::string& url);

  private:
    static std::size_t
    write_callback(void* contents, std::size_t size, std::size_t count, void* user_data);
    void cleanup() noexcept;
    [[nodiscard]] std::string
    request(const std::string& url, const std::string& method, const std::string& body);

    CURL* curl_ = nullptr;
    curl_slist* headers_ = nullptr;
    std::string auth_;
};

[[nodiscard]] std::string
http_post(const std::string& url, const std::string& body, const std::string& auth = {});
[[nodiscard]] std::string http_get(const std::string& url, const std::string& auth = {});
[[nodiscard]] std::string
http_put(const std::string& url, const std::string& body = "{}", const std::string& auth = {});

} // namespace cpgdb

#endif // CPGDB_UTILITIES_CURL_HPP
