#ifndef JANUS_GRAPH_HPP
#define JANUS_GRAPH_HPP

#include "data_access/IDatabase.hpp"
#include "utilities/curl.hpp"

class JanusGraph : public IDatabase {

public:
    virtual QueryResult evaluate(const Query& query) override{
        // ---- Query JanusGraph (Gremlin HTTP API)
        json gremlinReq;
        gremlinReq["gremlin"] = "graph.traversal().V().hasLabel('person').valueMap()";

        // All products with their properties
        gremlinReq["gremlin"] = "g.V().hasLabel('product').valueMap()";


        // Get all products in the "Electronics" category
        gremlinReq["gremlin"] = "g.V().has('category','name','Electronics').in('inCategory').valueMap()";
        
        gremlinReq["gremlin"] = query.query;

        // gremlinReq["gremlin"] = "g.V().has('product','productId','p1')."
        //         "bothE('alsoBought','coViewed','similarTo')."
        //         "order().by('weight', decr)."
        //         "otherV()."
        //         "dedup()."
        //         "valueMap()";

        //

        std::string janusResp = http_post(
            "http://localhost:8182/gremlin",
            gremlinReq.dump()
        );
        
        return QueryResult{{}, janusResp};//"JanusGraph Response:\n" + janusResp + "\n\n";
    }
};

#endif // JANUS_GRAPH_HPP