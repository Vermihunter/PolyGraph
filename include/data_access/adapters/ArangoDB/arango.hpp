#ifndef ARANGO_HPP
#define ARANGO_HPP

#include "data_access/IDatabase.hpp"
#include "utilities/curl.hpp"



// Debugging
#include <iostream>

class ArangoDB : public IDatabase {

public:
    /**
     * SAMPLE QUERIES:
     * - FOR u IN users RETURN u
     * - FOR o IN orders FILTER o.total > 200 RETURN o
     * - FOR u IN users FILTER u.name == "Alice" FOR o IN OUTBOUND u userPlacedOrder RETURN o
     * - FOR u IN users LET orders = (FOR o IN OUTBOUND u userPlacedOrder RETURN o) RETURN { user: u.name, orders: orders }
     * - FOR u IN users LET totalSpent = SUM(FOR o IN OUTBOUND u userPlacedOrder RETURN o.total) SORT totalSpent DESC LIMIT 1 RETURN { user: u.name, totalSpent: totalSpent }
     */
    virtual QueryResult evaluate(const Query& query) override {
        json aqlReq;
        aqlReq["query"] = query.query;

        std::string arangoResp = http_post(
            "http://localhost:8529/_api/cursor",
            aqlReq.dump(),
            "root:rootpassword"
        );

        json arangoJsonResp = json::parse(arangoResp);
        for(auto && result : arangoJsonResp["result"]) {
            for (auto& [key, value] : result.items()) {
                std::cout << key << " : " << value << std::endl;
            }
            std::cout << "-------" << std::endl;
        }

        return QueryResult{{}, arangoResp};//"ArangoDB Response:\n" + arangoResp;
    }
};

#endif // ARANGO_HPP