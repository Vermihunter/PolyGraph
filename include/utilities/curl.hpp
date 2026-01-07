#include <curl/curl.h>
#include "json.hpp"

using json = nlohmann::json;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

inline std::string http_post(const std::string& url, const std::string& body,
                      const std::string& auth = "")
{
    CURL* curl = curl_easy_init();
    std::string response;

    if (curl)
    {
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        if(!auth.empty())
            curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());

        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    return response;
}