#ifndef NEO4J_HPP
#define NEO4J_HPP

#include "data_access/IDatabase.hpp"
#include "utilities/curl.hpp"

class Neo4J : public IDatabase {

public:
    virtual QueryResult evaluate(const Query& query) override{
        json neoReq;
       
        neoReq["statements"] = json::array({
            {
                {"statement", "CREATE (n:Person {name: $name}) RETURN n"},
                {"parameters", {
                    {"name", "Alice"}
                }}
            }
        });

        neoReq["statements"] = json::array({
            {
                {"statement",
                "MATCH (p:Person)-[r:ACTED_IN]->(m:Movie) "
                "RETURN p.name AS actor, r.role AS role, m.title AS movie, m.released AS year "
                "ORDER BY movie"},
                {"parameters", json::object()}
            }
        });


        // MATCH path = (p:Person)-[:ACTED_IN]->(m:Movie) RETURN path
        // MATCH (m:Movie) RETURN m.title, m.released
        // MATCH path = (a:Person)-[:ACTED_IN]->(b:Movie) LIMIT 1 
        neoReq["statements"] = json::array({
            {
                {"statement", query.query},
                {"parameters", json::object()}
            }
        });

        std::string neoResp = http_post(
            "http://localhost:7474/db/neo4j/tx/commit",
            neoReq.dump()
        );

        json neoJsonResp = json::parse(neoResp);
        auto& results = neoJsonResp["results"];

        // take first result object
        auto& result = results[0];

        std::cout << "Columns: ";
        for (const auto& column : result["columns"]) {
            std::cout << column << " - ";
        }
        std::cout << std::endl;

        std::cout << "Row: " << std::endl;
        for (const auto& row : result["data"]) {
            for (const auto& field : row["row"]) {
                std::cout << field << " - ";
            }
            std::cout << std::endl;
        }
        std::cout << std::endl;


        
        return QueryResult{{}, neoResp};//"JanusGraph Response:\n" + janusResp + "\n\n";
    }
};

#endif // NEO4J_HPP