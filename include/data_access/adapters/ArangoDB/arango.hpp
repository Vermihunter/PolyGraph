#ifndef ARANGO_HPP
#define ARANGO_HPP

#include "data_access/IDatabase.hpp"
#include "utilities/curl.hpp"



// Debugging
#include <iostream>

class ArangoDB : public IDatabase {

public:
    virtual QueryResult evaluate(const Query& query) override {
        // ---- Query ArangoDB (AQL HTTP API)
        json aqlReq;
        aqlReq["query"] = query.query;
        // aqlReq["query"] = "FOR p IN people RETURN p";

        // // users/u1  --userPlacedOrder-->  orders/*
        // aqlReq["query"] = 
        //     "FOR v, e IN 1..1 OUTBOUND \"users/u1\" userPlacedOrder\n"
        //         "\tRETURN v";

        std::string arangoResp = http_post(
            "http://localhost:8529/_api/cursor",
            aqlReq.dump(),
            "root:rootpassword"
        );

        // std::cout << "Printing names" << std::endl;
        // json res = json::parse(arangoResp);
        // for (const auto& item : res["result"]) {
        //     std::cout << item["name"] << std::endl;
        // }
        return QueryResult{{}, arangoResp};//"ArangoDB Response:\n" + arangoResp;
    }
};

#endif // ARANGO_HPP