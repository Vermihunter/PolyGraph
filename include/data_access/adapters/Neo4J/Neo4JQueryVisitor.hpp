#ifndef NEO4J_QUERY_VISITOR_HPP
#define NEO4J_QUERY_VISITOR_HPP


#include <data_access/adapters/Neo4J/Neo4JConfig.hpp>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>
#include <utilities/uuid.hpp>

#include <query/query_types/GetNeighborsForVertexQuery.hpp>
#include <query/query_types/GetVertexByIdQuery.hpp>

class Neo4JQueryVisitor : public QueryVisitor {
    Neo4JConfig config_;

    const std::string getAllEdgesStrQuery =
        "MATCH ()-[r]->()\n"
        "RETURN {\n"
        "    id: elementId(r),\n"
        "    type: type(r),\n"
        "    start: elementId(startNode(r)),\n"
        "    end: elementId(endNode(r)),\n"
        "    properties: properties(r)\n"
        "} AS relationship;";

    const std::string getAllVerticesStrQuery =
        "MATCH (n)\n"
        "RETURN {\n"
        "   id: elementId(n),\n"
        "   labels: labels(n),\n"
        "   properties: properties(n)\n"
        "} AS node;";

    const std::string getVertexByIdStrQuery =
        "MATCH (n)\n"
        "WHERE n.uuid = $globalNodeId\n"
        "RETURN {\n"
        "   id: elementId(n),\n"
        "   uuid: n.uuid,\n"
        "   labels: labels(n),\n"
        "   properties: properties(n)\n"
        "} AS node;";

    const std::string getNeighborsForVertexStrQuery =
        "MATCH (n)<-[]->(neighbor)\n"
        "WHERE n.uuid = $globalNodeId\n"
        "RETURN {\n"
        "   id: elementId(neighbor),\n"
        "   uuid: neighbor.uuid,\n"
        "   labels: labels(neighbor),\n"
        "   properties: properties(neighbor)\n"
        "} AS node;";
public:
    explicit Neo4JQueryVisitor(DatabaseContext& db_context, Neo4JConfig&& config)
        : QueryVisitor(db_context), config_(std::move(config)) {}

    query_result_t visit(const GetAllVerticesQuery& q) override {
        return evaluate(getAllVerticesStrQuery, PARSE_TYPE::NODE, json::object());
    }

    query_result_t visit(const GetAllEdgesQuery& q) override {
        return evaluate(getAllEdgesStrQuery, PARSE_TYPE::RELATIONSHIP, json::object());
    }

    query_result_t visit(const GetNeighborsForVertexQuery& q) override {
        return evaluate(getNeighborsForVertexStrQuery, PARSE_TYPE::NODE, {
            {"globalNodeId", boost::uuids::to_string((q.getNode().global_id))}
        });
    }

    query_result_t visit(const GetVertexByIdQuery& q) override {
        return evaluate(getVertexByIdStrQuery, PARSE_TYPE::NODE, {
            {"globalNodeId", boost::uuids::to_string((q.getNode().global_id))}
        });
    }

private:
    json send_req(const std::string& query, const json& parameters) const {
        const json req {
            {"statements", json::array({
                {
                    {"statement", query},
                    {"parameters", parameters},
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

    [[nodiscard]] query_result_t evaluate(const std::string& query, PARSE_TYPE parse_type, const json& parameters) const {
        json response = send_req(query, parameters);
        return config_.response_processor->process(response, parse_type);
    }
};


#endif // NEO4J_QUERY_VISITOR_HPP