/**
 * @file src/utilities/curl.cpp
 * @brief Implementation for curl in the utility module.
 */

#include "utilities/curl.hpp"

#include <stdexcept>
#include <utility>

namespace cpgdb {

    CurlHttpClient::CurlHttpClient(std::string auth) : curl_(curl_easy_init()), auth_(std::move(auth)) {
        if (!curl_) {
            throw std::runtime_error("curl_easy_init() failed");
        }

        headers_ = curl_slist_append(nullptr, "Content-Type: application/json");
        if (!headers_) {
            cleanup();
            throw std::runtime_error("curl_slist_append() failed");
        }
    }

    CurlHttpClient::CurlHttpClient(CurlHttpClient &&other) noexcept :
        curl_(std::exchange(other.curl_, nullptr)), headers_(std::exchange(other.headers_, nullptr)),
        auth_(std::move(other.auth_)) {}

    CurlHttpClient &CurlHttpClient::operator=(CurlHttpClient &&other) noexcept {
        if (this != &other) {
            cleanup();
            curl_ = std::exchange(other.curl_, nullptr);
            headers_ = std::exchange(other.headers_, nullptr);
            auth_ = std::move(other.auth_);
        }
        return *this;
    }

    CurlHttpClient::~CurlHttpClient() { cleanup(); }

    std::string CurlHttpClient::post(const std::string &url, const std::string &body) {
        return request(url, "POST", body);
    }

    std::string CurlHttpClient::put(const std::string &url, const std::string &body) {
        return request(url, "PUT", body);
    }

    std::string CurlHttpClient::get(const std::string &url) { return request(url, "GET", {}); }

    std::size_t CurlHttpClient::write_callback(void *contents, std::size_t size, std::size_t count, void *user_data) {
        const std::size_t bytes = size * count;
        static_cast<std::string *>(user_data)->append(static_cast<char *>(contents), bytes);
        return bytes;
    }

    void CurlHttpClient::cleanup() noexcept {
        if (headers_) {
            curl_slist_free_all(headers_);
            headers_ = nullptr;
        }
        if (curl_) {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
    }

    std::string CurlHttpClient::request(const std::string &url, const std::string &method, const std::string &body) {
        if (!curl_) {
            throw std::logic_error("CurlHttpClient used after being moved from");
        }

        std::string response;
        curl_easy_reset(curl_);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &CurlHttpClient::write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

        if (!auth_.empty()) {
            curl_easy_setopt(curl_, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl_, CURLOPT_USERPWD, auth_.c_str());
        }

        if (method == "POST") {
            curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        } else if (method == "PUT") {
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT");
        } else if (method != "GET") {
            throw std::invalid_argument("Unsupported HTTP method: " + method);
        }

        if (method != "GET") {
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }

        const CURLcode result = curl_easy_perform(curl_);
        if (result != CURLE_OK) {
            throw std::runtime_error("HTTP " + method + " failed: " + curl_easy_strerror(result));
        }

        long status = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
        if (status < 200 || status >= 300) {
            throw std::runtime_error("HTTP " + method + " returned status " + std::to_string(status) + ": " + response);
        }

        return response;
    }

    std::string http_post(const std::string &url, const std::string &body, const std::string &auth) {
        return CurlHttpClient(auth).post(url, body);
    }

    std::string http_get(const std::string &url, const std::string &auth) { return CurlHttpClient(auth).get(url); }

    std::string http_put(const std::string &url, const std::string &body, const std::string &auth) {
        return CurlHttpClient(auth).put(url, body);
    }

} // namespace cpgdb
