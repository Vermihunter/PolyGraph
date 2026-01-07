#ifndef ARANGO_HPP
#define ARANGO_HPP

#include "data_access/IDatabase.hpp"
#include "utilities/curl.hpp"

class ArangoDB : public IDatabase {

public:
    virtual std::string evaluate(const std::string& query) override {
        // ---- Query ArangoDB (AQL HTTP API)
        json aqlReq;
        aqlReq["query"] = "FOR p IN people RETURN p";

        std::string arangoResp = http_post(
            "http://localhost:8529/_api/cursor",
            aqlReq.dump(),
            "root:rootpassword"
        );
        
        return "ArangoDB Response:\n" + arangoResp;
    }
};

#endif // ARANGO_HPP