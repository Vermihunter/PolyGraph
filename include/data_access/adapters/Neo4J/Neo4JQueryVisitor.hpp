#ifndef NEO4J_QUERY_VISITOR_HPP
#define NEO4J_QUERY_VISITOR_HPP

#include <iostream>

#include <data_access/adapters/Neo4J/Neo4JConfig.hpp>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <query/query_types/GetAllVerticesQuery.hpp>
#include <query/query_types/GetAllEdgesQuery.hpp>
#include <serialization/DataModelJsonSerializer.hpp>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>

class Neo4JQueryVisitor : public QueryVisitor {
    Neo4JConfig config_;
public:
    explicit Neo4JQueryVisitor(Neo4JConfig&& config) : config_(std::move(config)) {}

    query_result_t visit(const GetAllVerticesQuery& q) override {
        const std::string strQuery =
            "MATCH (n)\n"
            "RETURN {\n"
            "   id: elementId(n),\n"
            "   labels: labels(n),\n"
            "   properties: properties(n)\n"
            "} AS node;";

        return evaluate(strQuery, PARSE_TYPE::NODE);
    }

    query_result_t visit(const GetAllEdgesQuery& q) override {
        const std::string strQuery =
            "MATCH ()-[r]->()\n"
            "RETURN {\n"
            "    id: elementId(r),\n"
            "    type: type(r),\n"
            "    start: elementId(startNode(r)),\n"
            "    end: elementId(endNode(r)),\n"
            "    properties: properties(r)\n"
            "} AS relationship;";

        return evaluate(strQuery, PARSE_TYPE::RELATIONSHIP);
    }

private:
    json send_req(const std::string& query) const {
        const json req {
            {"statements", json::array({
                {
                    {"statement", query},
                    {"parameters", json::object()},
                    //{"resultDataContents", json::array({"graph"})}
                }
            })}
        };

        std::string res = http_post(
            config_.endpoint,
            req.dump()
        );

        return json::parse(res);
    }

    query_result_t evaluate(const std::string& query, PARSE_TYPE parse_type) {
        json response = send_req(query);
        return config_.response_processor->process(response, parse_type);
    }
};


#endif // NEO4J_QUERY_VISITOR_HPP