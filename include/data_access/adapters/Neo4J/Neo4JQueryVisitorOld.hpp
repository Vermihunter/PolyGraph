#ifndef NEO4J_QUERY_VISITOR_HPP
#define NEO4J_QUERY_VISITOR_HPP


#include <data_access/adapters/Neo4J/Neo4JConfig.hpp>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>
#include <utilities/uuid.hpp>

#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <sstream>

#include <query/query_types/GetNeighborsForVertexQuery.hpp>
#include <query/query_types/GetVertexByIdQuery.hpp>

#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "response_processors/LocallyDeployedNeo4JResponseProcessor.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/Profiling.hpp"

class Neo4JQueryVisitor : public QueryVisitor {
    const BackendConnectionConfig& config_;
    LocallyDeployedNeo4JResponseProcessor response_processor_;

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
     Neo4JQueryVisitor(DatabaseContext& db_context, const BackendConnectionConfig& config)
        : QueryVisitor(db_context), config_(config), response_processor_(db_context) {}

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

    /*
    query_result_t visit(const GetNeighborsBatchQuery& q) override {
        CPGDB_PROFILE_FUNCTION();
        json frontier = json::array();

        for (const auto& v : q.frontier()) {
            frontier.push_back(boost::uuids::to_string(v.global_id));
        }

        json edgeTypes = json::array();

        if (q.spec().allowed_edge_types.empty()) {
            for (auto edge_type_id : db_context_.backend_registry
                     ->descriptor(NEO4J)
                     .coverage.edge_types) {
                edgeTypes.push_back(db_context_.schema->edge_type(edge_type_id).name);
            }
        } else {
            for (auto edge_type_id : q.spec().allowed_edge_types) {
                edgeTypes.push_back(db_context_.schema->edge_type(edge_type_id).name);
            }
        }

        std::string relationship_pattern;

        switch (q.spec().direction) {
            case Direction::Directed:
                relationship_pattern = "(source)-[r]->(neighbor)";
                break;
            case Direction::Undirected:
            case Direction::Both:
                relationship_pattern = "(source)-[r]-(neighbor)";
                break;
        }

         const std::string query =
             "UNWIND $frontier AS source_uuid "
             "MATCH " + relationship_pattern + " "
             "WHERE source.uuid = source_uuid "
             "AND (size($edgeTypes) = 0 OR type(r) IN $edgeTypes) "
             "RETURN "
             "{ "
             "   id: elementId(source), "
             "   uuid: source.uuid, "
             "   type: head(labels(source)), "
             "   properties: properties(source), "
             "   labels: labels(source) "
             "} AS source, "
             "{ "
             "   id: elementId(neighbor), "
             "   uuid: neighbor.uuid, "
             "   type: head(labels(neighbor)), "
             "   properties: properties(neighbor), "
             "   labels: labels(neighbor) "
             "} AS neighbor, "
             "{ "
             "   start: startNode(r).uuid, "
             "   end: endNode(r).uuid, "
             "   id: elementId(r), "
             "   type: type(r), "
             "   uuid: r.uuid, "
             "   properties: properties(r) "
             "} AS edge;";

        return evaluate(query, PARSE_TYPE::ADJACENCY_ROW, {
            {"frontier", frontier},
            {"edgeTypes", edgeTypes}
        });
    }
    */
    query_result_t visit(const GetNeighborsBatchQuery& q) override {
        CPGDB_PROFILE_FUNCTION();

        json frontier = json::array();
        for (const auto& v : q.frontier()) {
            frontier.push_back(boost::uuids::to_string(v.global_id));
        }

        const auto edge_type_names = edge_type_names_for_spec(q.spec());
        const std::string edge_match =
            build_edge_match(q.spec().direction, relationship_type_pattern(edge_type_names));

        const std::string source_type_expr =
            "coalesce("
            "CASE WHEN coalesce(source.is_proxy, false) "
            "THEN source.`remote-type` ELSE source.type END, "
            "head([label IN labels(source) "
            "WHERE label <> 'Proxy' AND label <> 'CPGDBVertex'])"
            ")";

        const std::string neighbor_type_expr =
            "coalesce("
            "CASE WHEN coalesce(neighbor.is_proxy, false) "
            "THEN neighbor.`remote-type` ELSE neighbor.type END, "
            "head([label IN labels(neighbor) "
            "WHERE label <> 'Proxy' AND label <> 'CPGDBVertex'])"
            ")";

        const std::string query =
            "UNWIND $frontier AS source_uuid "
            "MATCH (source {uuid: source_uuid}) "
            + edge_match +
            "RETURN "
            "{ "
            "   id: elementId(source), "
            "   uuid: source.uuid, "
            "   type: " + source_type_expr + ", "
            "   labels: labels(source), "
            "   properties: properties(source) "
            "} AS source, "
            "{ "
            "   id: elementId(neighbor), "
            "   uuid: coalesce(neighbor.`remote-uuid`, neighbor.uuid), "
            "   type: " + neighbor_type_expr + ", "
            "   labels: labels(neighbor), "
            "   properties: properties(neighbor) "
            "} AS neighbor, "
            "{ "
            "   start: startNode(r).uuid, "
            "   end: endNode(r).uuid, "
            "   id: elementId(r), "
            "   type: type(r), "
            "   uuid: r.uuid, "
            "   properties: properties(r) "
            "} AS edge";

        return evaluate(query, PARSE_TYPE::ADJACENCY_ROW, {
            {"frontier", frontier}
        });
    }

    std::vector<AdjacencyRow>
    get_neighbors_batch_fast(std::span<const NodeRef> frontier,
                             const ExpandSpec& spec) const {

        if (frontier.empty()) {
            return {};
        }

        std::vector<AdjacencyRow> rows;
        rows.reserve(frontier.size());

        std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>> row_by_source;
        row_by_source.reserve(frontier.size());

        json frontier_json = json::array();

        for (const auto& source : frontier) {
            frontier_json.push_back(boost::uuids::to_string(source.global_id));

            AdjacencyRow row;
            row.source = source;

            row_by_source.emplace(source.global_id, rows.size());
            rows.push_back(std::move(row));
        }

        json edge_types_json = json::array();
        for (const TypeId edge_type_id : spec.allowed_edge_types) {
            edge_types_json.push_back(db_context_.schema->edge_type(edge_type_id).name);
        }

        std::string edge_pattern;
        switch (spec.direction) {
            case Direction::Directed:
                edge_pattern = "MATCH (source)-[r]->(neighbor) ";
                break;
            case Direction::Undirected:
            case Direction::Both:
                edge_pattern = "MATCH (source)-[r]-(neighbor) ";
                break;
        }

        /*
         * Keep the fast scalar response shape, but do NOT use labels(neighbor)[0]
         * as the logical type.
         *
         * Cross-backend endpoints are stored in Neo4J as (:Proxy) nodes:
         *   { uuid: realUuid, type: "Proxy", is_proxy: true,
         *     `remote-type`: "CourseOffering" }
         *
         * For BFS, the returned NodeRef must use the logical remote type, not
         * the physical :Proxy label/type. Otherwise the next layer is planned
         * as if the vertex were a Neo4J-local Proxy node and cross-backend BFS
         * collapses to "only inside Neo4J".
         */
        const std::string query =
            "UNWIND $frontier AS source_uuid "
            "MATCH (source {uuid: source_uuid}) " +
            edge_pattern +
            "WHERE (size($edgeTypes) = 0 OR type(r) IN $edgeTypes) "
            "RETURN "
            "source.uuid AS source_uuid, "
            "neighbor.uuid AS neighbor_uuid, "
            "coalesce(neighbor.is_proxy, false) AS neighbor_is_proxy, "
            "neighbor.`remote-type` AS neighbor_remote_type, "
            "neighbor.type AS neighbor_type_property, "
            "head(labels(neighbor)) AS neighbor_label, "
            "r.uuid AS edge_uuid, "
            "type(r) AS edge_type, "
            "startNode(r).uuid AS edge_from_uuid, "
            "endNode(r).uuid AS edge_to_uuid";

        const json parameters{
            {"frontier", frontier_json},
            {"edgeTypes", edge_types_json}
        };

        const json response = send_req(query, parameters);

        if (response.contains("errors") && !response["errors"].empty()) {
            throw std::runtime_error(
                "Neo4J fast neighbor query failed: " + response["errors"].dump()
            );
        }

        if (!response.contains("results") ||
            response["results"].empty() ||
            !response["results"][0].contains("data")) {
            return rows;
        }

        const auto& data = response["results"][0]["data"];

        std::unordered_map<
            GlobalId,
            std::unordered_set<GlobalId, boost::hash<GlobalId>>,
            boost::hash<GlobalId>
        > seen_by_source;

        for (const auto& item : data) {
            if (!item.contains("row") || !item["row"].is_array()) {
                continue;
            }

            const auto& r = item["row"];
            if (r.size() < 10 || r[0].is_null() || r[1].is_null()) {
                continue;
            }

            auto string_opt = [](const json& value) -> std::optional<std::string> {
                if (value.is_string()) {
                    auto result = value.get<std::string>();
                    if (!result.empty()) {
                        return result;
                    }
                }
                return std::nullopt;
            };

            auto bool_or_false = [](const json& value) -> bool {
                return value.is_boolean() && value.get<bool>();
            };

            auto printable = [](const std::string& value) {
                std::ostringstream oss;
                for (unsigned char c : value) {
                    if (c == '\0') {
                        oss << "\\0";
                    } else {
                        oss << static_cast<char>(c);
                    }
                }
                return oss.str();
            };

            auto resolve_logical_type = [&](const json& is_proxy_value,
                                            const json& remote_type_value,
                                            const json& type_property_value,
                                            const json& label_value,
                                            const std::string& neighbor_uuid) {
                const bool is_proxy_flag = bool_or_false(is_proxy_value);
                const auto remote_type = string_opt(remote_type_value);
                const auto type_property = string_opt(type_property_value);
                const auto label = string_opt(label_value);

                const bool looks_like_proxy =
                    is_proxy_flag ||
                    (type_property.has_value() && *type_property == "Proxy") ||
                    (label.has_value() && *label == "Proxy");

                if (looks_like_proxy && remote_type.has_value()) {
                    return *remote_type;
                }

                if (type_property.has_value() && *type_property != "Proxy") {
                    return *type_property;
                }

                if (label.has_value() && *label != "Proxy") {
                    return *label;
                }

                if (type_property.has_value()) {
                    return *type_property;
                }

                if (label.has_value()) {
                    return *label;
                }

                throw std::runtime_error(
                    "Neo4J fast path could not resolve logical type for neighbor uuid=" +
                    neighbor_uuid +
                    " is_proxy=" + is_proxy_value.dump() +
                    " remote_type=" + remote_type_value.dump() +
                    " type_property=" + type_property_value.dump() +
                    " label=" + label_value.dump()
                );
            };

            const std::string source_uuid_str = r[0].get<std::string>();
            const std::string neighbor_uuid_str = r[1].get<std::string>();
            const std::string neighbor_type_name = resolve_logical_type(
                r[2], r[3], r[4], r[5], neighbor_uuid_str
            );
            const std::string edge_uuid_str = r[6].is_null() ? "" : r[6].get<std::string>();
            const std::string edge_type_name = r[7].get<std::string>();
            const std::string edge_from_uuid_str = r[8].get<std::string>();
            const std::string edge_to_uuid_str = r[9].get<std::string>();

            if (neighbor_type_name.find('\0') != std::string::npos) {
                throw std::runtime_error(
                    "Neo4J fast path resolved invalid logical type containing NUL bytes for neighbor uuid=" +
                    neighbor_uuid_str + ": '" + printable(neighbor_type_name) + "'"
                );
            }

            const GlobalId source_id =
                boost::lexical_cast<boost::uuids::uuid>(source_uuid_str);

            const GlobalId neighbor_id =
                boost::lexical_cast<boost::uuids::uuid>(neighbor_uuid_str);

            auto row_it = row_by_source.find(source_id);
            if (row_it == row_by_source.end()) {
                continue;
            }

            auto& seen = seen_by_source[source_id];
            if (!seen.insert(neighbor_id).second) {
                continue;
            }

            const TypeId neighbor_type =
                db_context_.schema->node_type_id(neighbor_type_name);
            if (!spec.allows_target_type(neighbor_type)) {
                continue;
            }

            const TypeId edge_type =
                db_context_.schema->edge_type_id(edge_type_name);
            if (!spec.allows_edge_type(edge_type)) {
                continue;
            }

            NodeRef neighbor_ref{
                .global_id = neighbor_id,
                .type = neighbor_type
            };

            NeighborRef neighbor{
                .vertex = neighbor_ref,
                .via_edge = std::nullopt
            };

            if (!edge_uuid_str.empty()) {
                EdgeRef edge_ref{
                    .global_id = boost::lexical_cast<boost::uuids::uuid>(edge_uuid_str),
                    .type = edge_type,
                    .from = boost::lexical_cast<boost::uuids::uuid>(edge_from_uuid_str),
                    .to = boost::lexical_cast<boost::uuids::uuid>(edge_to_uuid_str)
                };

                neighbor.via_edge = edge_ref;
            }

            rows[row_it->second].neighbors.push_back(std::move(neighbor));
        }

        return rows;
    }

private:
    /*
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
    */

    json send_req(const std::string& query, const json& parameters) const {
        CPGDB_PROFILE_FUNCTION();

        json req;

        {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: build request JSON");

            req = json{
                {"statements", json::array({
                    {
                        {"statement", query},
                        {"parameters", parameters}
                    }
                })}
            };
        }

        std::string request_body;

        {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: dump request JSON");
            request_body = req.dump();
        }

        std::string res;

        {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: HTTP POST");
            res = http_post(config_.endpoint, request_body);
        }

        json parsed;

        {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: parse response JSON");
            parsed = json::parse(res);
        }

        return parsed;
    }

    [[nodiscard]] query_result_t evaluate(const std::string& query, PARSE_TYPE parse_type, const json& parameters) const {
        json response = send_req(query, parameters);
        return response_processor_.process(response, parse_type);
    }
};


#endif // NEO4J_QUERY_VISITOR_HPP
