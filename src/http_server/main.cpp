#include <drogon/drogon.h>

#include "cli/CurlGlobalGuard.hpp"
#include "server/controllers/TraversalController.hpp"

int main(int argc, char** argv) {
    cpgdb::runtime::CurlGlobalGuard curl;

    drogon::app().registerHandler(
        "/healthz",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            Json::Value body;
            body["status"] = "ok";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
            callback(resp);
        },
        {drogon::Get}
    );

    const std::string configPath =
        argc >= 2 ? argv[1] : "../../config/http_server.full.json";

    drogon::app()
        .loadConfigFile(configPath)
        .run();
}