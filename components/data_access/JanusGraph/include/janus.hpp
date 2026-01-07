#ifndef ARANGO_HPP
#define ARANGO_HPP

#include "data_access/IDatabase.hpp"
#include "utilities/curl.hpp"

class JanusGraph : public IDatabase {

public:
    virtual std::string evaluate(const std::string& query) override {
        // ---- Query JanusGraph (Gremlin HTTP API)
        json gremlinReq;
        gremlinReq["gremlin"] = "graph.traversal().V().hasLabel('person').valueMap()";

        std::string janusResp = http_post(
            "http://localhost:8182/gremlin",
            gremlinReq.dump()
        );
        
        return "JanusGraph Response:\n" + janusResp + "\n\n";
    }
};

#endif // ARANGO_HPP