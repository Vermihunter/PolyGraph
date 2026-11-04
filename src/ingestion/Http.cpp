#include "Native.hpp"
#if POLYGRAPH_INGESTION_HAS_CURL
#include <curl/curl.h>
#include <memory>
namespace polygraph::ingestion::detail {
    Json http_json(const Json &target, const std::string &method, const std::string &path, const std::string &body,
                   bool allow_not_found) {
        static const bool initialized = [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
                throw Error("curl initialization failed");
            return true;
        }();
        (void) initialized;
        const auto endpoint = target.at("endpoint").get<std::string>();
        if (!endpoint.starts_with("http://") && !endpoint.starts_with("https://"))
            throw Error("HTTP endpoint must use http:// or https://");
        if (endpoint.find('@') != std::string::npos)
            throw Error("Do not embed credentials in an HTTP URL; use username and password_env");
        const auto url = endpoint + path;
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl)
            throw Error("Cannot allocate curl handle");
        std::string response;
        struct Receiver {
            std::string *text;
            bool overflow = false;
        } receiver{&response};
        const auto receive = +[](char *data, std::size_t size, std::size_t count, void *user) -> std::size_t {
            auto &r = *static_cast<Receiver *>(user);
            const auto bytes = size * count;
            if (bytes > 64 * 1024 * 1024 || r.text->size() > 64 * 1024 * 1024 - bytes) {
                r.overflow = true;
                return 0;
            }
            try {
                r.text->append(data, bytes);
            } catch (...) {
                return 0;
            }
            return bytes;
        };
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &receiver);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(target.value("timeout_seconds", 900)));
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        const auto username = target.value("username", std::string{}),
                   password = environment_secret(target, "password_env");
        curl_easy_setopt(curl.get(), CURLOPT_USERNAME, username.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, password.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        if (method == "POST" || method == "PUT" || method == "PATCH") {
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
                curl_slist_append(nullptr, "Content-Type: application/json; charset=utf-8"), curl_slist_free_all);
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        const auto status = curl_easy_perform(curl.get());
        if (status != CURLE_OK)
            throw Error(std::string("HTTP import request failed: ") + curl_easy_strerror(status) +
                        (receiver.overflow ? " (response exceeded 64 MiB)" : ""));
        long code = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
        if (code == 404 && allow_not_found)
            return {{"not_found", true}};
        if (code < 200 || code >= 300)
            throw Error("HTTP import request returned status " + std::to_string(code) + " for " + path);
        try {
            auto result = Json::parse(response);
            if (result.value("error", false))
                throw Error("Backend reported HTTP API error");
            return result;
        } catch (const Json::exception &e) {
            throw Error(std::string("Invalid HTTP JSON response: ") + e.what());
        }
    }
} // namespace polygraph::ingestion::detail
#else
namespace polygraph::ingestion::detail {
    Json http_json(const Json &, const std::string &, const std::string &, const std::string &, bool) {
        throw Error("HTTP native executors require libcurl; rebuild with native executors enabled");
    }
} // namespace polygraph::ingestion::detail
#endif
