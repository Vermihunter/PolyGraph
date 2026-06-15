#ifndef DB_CURL_UTIL_HPP
#define DB_CURL_UTIL_HPP

#include <curl/curl.h>

#include <stdexcept>
#include <string>
#include <utility>

#include "json.hpp"

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

class CurlHttpClient {
public:
    explicit CurlHttpClient(std::string auth = {})
        : auth_(std::move(auth)) {
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("curl_easy_init() failed");
        }

        headers_ = curl_slist_append(headers_, "Content-Type: application/json");
        if (!headers_) {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
            throw std::runtime_error("curl_slist_append() failed");
        }
    }

    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;

    CurlHttpClient(CurlHttpClient&& other) noexcept
        : curl_(other.curl_), headers_(other.headers_), auth_(std::move(other.auth_)) {
        other.curl_ = nullptr;
        other.headers_ = nullptr;
    }

    CurlHttpClient& operator=(CurlHttpClient&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        cleanup();
        curl_ = other.curl_;
        headers_ = other.headers_;
        auth_ = std::move(other.auth_);
        other.curl_ = nullptr;
        other.headers_ = nullptr;
        return *this;
    }

    ~CurlHttpClient() {
        cleanup();
    }

    std::string post(const std::string& url, const std::string& body) {
        return request(url, "POST", body);
    }

    std::string put(const std::string& url, const std::string& body = "{}") {
        return request(url, "PUT", body);
    }

    std::string get(const std::string& url) {
        return request(url, "GET", "");
    }

private:
    CURL* curl_ = nullptr;
    curl_slist* headers_ = nullptr;
    std::string auth_;

    void cleanup() noexcept {
        if (headers_) {
            curl_slist_free_all(headers_);
            headers_ = nullptr;
        }
        if (curl_) {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
    }

    std::string request(const std::string& url,
                        const std::string& method,
                        const std::string& body) {
        if (!curl_) {
            throw std::runtime_error("CurlHttpClient used after cleanup");
        }

        std::string response;

        curl_easy_reset(curl_);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);

        if (!auth_.empty()) {
            curl_easy_setopt(curl_, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl_, CURLOPT_USERPWD, auth_.c_str());
        }

        if (method == "POST") {
            curl_easy_setopt(curl_, CURLOPT_POST, 1L);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else if (method == "PUT") {
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else if (method != "GET") {
            throw std::runtime_error("Unsupported HTTP method: " + method);
        }

        CURLcode code = curl_easy_perform(curl_);

        long http_status = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_status);

        if (code != CURLE_OK) {
            throw std::runtime_error(
                "HTTP " + method + " failed: " + std::string(curl_easy_strerror(code))
            );
        }

        if (http_status < 200 || http_status >= 300) {
            throw std::runtime_error(
                "HTTP " + method + " returned status " + std::to_string(http_status) +
                ": " + response
            );
        }

        return response;
    }
};

inline std::string http_post(const std::string& url,
                             const std::string& body,
                             const std::string& auth = "")
{
    CurlHttpClient client(auth);
    return client.post(url, body);
}

inline std::string http_get(const std::string& url, const std::string& auth = "") {
    CurlHttpClient client(auth);
    return client.get(url);
}

inline std::string http_put(const std::string& url,
                            const std::string& body = "{}",
                            const std::string& auth = "")
{
    CurlHttpClient client(auth);
    return client.put(url, body);
}

#endif // DB_CURL_UTIL_HPP
