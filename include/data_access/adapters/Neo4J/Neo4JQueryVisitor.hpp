#ifndef NEO4J_QUERY_VISITOR_HPP
#define NEO4J_QUERY_VISITOR_HPP

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container_hash/hash.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <data_access/adapters/Neo4J/Neo4JConfig.hpp>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <query/query_types/GetAllEdgesQuery.hpp>
#include <query/query_types/GetAllVerticesQuery.hpp>
#include <query/query_types/GetNeighborsBatchQuery.hpp>
#include <query/query_types/GetNeighborsForVertexQuery.hpp>
#include <query/query_types/GetVertexByIdQuery.hpp>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>
#include <utilities/uuid.hpp>

#include "data_model/AdjacencyRow.hpp"
#include "data_model/MinimalAdjacencyRow.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "response_processors/LocallyDeployedNeo4JResponseProcessor.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/Profiling.hpp"

class Neo4JQueryVisitor : public QueryVisitor {
    const BackendConnectionConfig& config_;
    LocallyDeployedNeo4JResponseProcessor response_processor_;
    mutable CurlHttpClient http_;

    std::unordered_map<std::string, TypeId> node_type_cache_;
    std::unordered_map<std::string, TypeId> edge_type_cache_;
    mutable std::unordered_map<std::string, GlobalId> uuid_cache_;

    static std::string logical_type_expr(const std::string& var) {
        return
            "coalesce("
            + var + ".logical_type, "
            "CASE WHEN coalesce(" + var + ".is_proxy, false) "
            "THEN " + var + ".`remote-type` ELSE null END, "
            "head([label IN labels(" + var + ") "
            "WHERE label <> 'Proxy' "
            "AND label <> 'CPGDBVertex' "
            "AND label <> '__GraphNode']), "
            "CASE WHEN " + var + ".type <> 'Proxy' THEN " + var + ".type ELSE null END"
            ")";
    }

    static std::string logical_uuid_expr(const std::string& var) {
        return "coalesce(" + var + ".`remote-uuid`, " + var + ".uuid)";
    }

    const std::string getAllEdgesStrQuery =
        "MATCH ()-[r]->()\n"
        "RETURN {\n"
        "    id: elementId(r),\n"
        "    type: coalesce(r.logical_type, r.type, type(r)),\n"
        "    uuid: r.uuid,\n"
        "    start: coalesce(startNode(r).`remote-uuid`, startNode(r).uuid),\n"
        "    end: coalesce(endNode(r).`remote-uuid`, endNode(r).uuid),\n"
        "    properties: properties(r)\n"
        "} AS relationship;";

    const std::string getAllVerticesStrQuery =
        "MATCH (n)\n"
        "RETURN {\n"
        "   id: elementId(n),\n"
        "   uuid: coalesce(n.`remote-uuid`, n.uuid),\n"
        "   type: " + logical_type_expr("n") + ",\n"
        "   labels: labels(n),\n"
        "   properties: properties(n)\n"
        "} AS node;";

    const std::string getVertexByIdStrQuery =
        "MATCH (n)\n"
        "WHERE n.uuid = $globalNodeId OR n.`remote-uuid` = $globalNodeId\n"
        "RETURN {\n"
        "   id: elementId(n),\n"
        "   uuid: coalesce(n.`remote-uuid`, n.uuid),\n"
        "   type: " + logical_type_expr("n") + ",\n"
        "   labels: labels(n),\n"
        "   properties: properties(n)\n"
        "} AS node;";

    const std::string getNeighborsForVertexStrQuery =
        "MATCH (n)\n"
        "WHERE n.uuid = $globalNodeId OR n.`remote-uuid` = $globalNodeId\n"
        "MATCH (n)-[]-(neighbor)\n"
        "RETURN {\n"
        "   id: elementId(neighbor),\n"
        "   uuid: coalesce(neighbor.`remote-uuid`, neighbor.uuid),\n"
        "   type: " + logical_type_expr("neighbor") + ",\n"
        "   labels: labels(neighbor),\n"
        "   properties: properties(neighbor)\n"
        "} AS node;";

public:
    Neo4JQueryVisitor(DatabaseContext& db_context,
                      const BackendConnectionConfig& config)
        : QueryVisitor(db_context),
          config_(config),
          response_processor_(db_context),
          http_(config.credentials.has_value()
                    ? config.credentials->username + ":" + config.credentials->password
                    : "") {
        for (const auto& [id, type] : db_context_.schema->node_types()) {
            node_type_cache_.emplace(type.name, id);
        }

        for (const auto& [id, type] : db_context_.schema->edge_types()) {
            edge_type_cache_.emplace(type.name, id);
        }
    }

    query_result_t visit(const GetAllVerticesQuery&) override {
        return evaluate(getAllVerticesStrQuery, PARSE_TYPE::NODE, json::object());
    }

    query_result_t visit(const GetAllEdgesQuery&) override {
        return evaluate(getAllEdgesStrQuery, PARSE_TYPE::RELATIONSHIP, json::object());
    }

    query_result_t visit(const GetNeighborsForVertexQuery& q) override {
        return evaluate(getNeighborsForVertexStrQuery, PARSE_TYPE::NODE, {
            {"globalNodeId", boost::uuids::to_string(q.getNode().global_id)}
        });
    }

    query_result_t visit(const GetVertexByIdQuery& q) override {
        return evaluate(getVertexByIdStrQuery, PARSE_TYPE::NODE, {
            {"globalNodeId", boost::uuids::to_string(q.getNode().global_id)}
        });
    }

    query_result_t visit(const GetNeighborsBatchQuery& q) override {
        CPGDB_PROFILE_FUNCTION();

        json frontier = json::array();
        for (const auto& v : q.frontier()) {
            frontier.push_back(boost::uuids::to_string(v.global_id));
        }

        const auto edge_type_names = edge_type_names_for_spec(q.spec());
        const std::string edge_match =
            build_edge_match(q.spec().direction, relationship_type_pattern(edge_type_names));

        const std::string query =
            "UNWIND $frontier AS source_uuid "
            "MATCH (source {uuid: source_uuid}) "
            + edge_match +
            "RETURN "
            "{ "
            "   id: elementId(source), "
            "   uuid: " + logical_uuid_expr("source") + ", "
            "   type: " + logical_type_expr("source") + ", "
            "   labels: labels(source), "
            "   properties: properties(source) "
            "} AS source, "
            "{ "
            "   id: elementId(neighbor), "
            "   uuid: " + logical_uuid_expr("neighbor") + ", "
            "   type: " + logical_type_expr("neighbor") + ", "
            "   labels: labels(neighbor), "
            "   properties: properties(neighbor) "
            "} AS neighbor, "
            "{ "
            "   start: " + logical_uuid_expr("startNode(r)") + ", "
            "   end: " + logical_uuid_expr("endNode(r)") + ", "
            "   id: elementId(r), "
            "   type: coalesce(r.logical_type, r.type, type(r)), "
            "   uuid: r.uuid, "
            "   properties: properties(r) "
            "} AS edge";

        return evaluate(query, PARSE_TYPE::ADJACENCY_ROW, {
            {"frontier", frontier}
        });
    }

    std::vector<MinimalAdjacencyRow>
    get_minimal_neighbors_batch_fast(std::span<const NodeRef> frontier,
                                     const ExpandSpec& spec) const {
        CPGDB_PROFILE_FUNCTION();

        if (frontier.empty()) {
            return {};
        }

        std::vector<MinimalAdjacencyRow> rows;
        rows.reserve(frontier.size());

        std::unordered_map<std::string, std::size_t> row_by_source_uuid;
        row_by_source_uuid.reserve(frontier.size());

        for (const auto& source : frontier) {
            std::string source_uuid = boost::uuids::to_string(source.global_id);
            auto [it, inserted] = row_by_source_uuid.emplace(source_uuid, rows.size());

            if (inserted) {
                rows.push_back(MinimalAdjacencyRow{
                    .source = source,
                    .neighbors = {}
                });
            }
        }

        const auto grouped_frontier = group_frontier_by_physical_source_label(frontier);
        const auto edge_type_names = edge_type_names_for_spec(spec);
        const std::string type_pattern = relationship_type_pattern(edge_type_names);

        for (const auto& [source_label, group] : grouped_frontier) {
            if (group.empty()) {
                continue;
            }

            json frontier_json = json::array();
            for (const auto& source : group) {
                frontier_json.push_back(boost::uuids::to_string(source.global_id));
            }

            const std::string edge_match = build_edge_match(spec.direction, type_pattern);

            std::string query =
                "UNWIND $frontier AS source_uuid "
                "MATCH (source:" + escape_cypher_identifier(source_label) + " {uuid: source_uuid}) "
                + edge_match +
                "RETURN "
                + logical_uuid_expr("source") + " AS source_uuid, "
                + logical_uuid_expr("neighbor") + " AS neighbor_uuid, "
                "neighbor.type_id AS neighbor_type_id, "
                + logical_type_expr("neighbor") + " AS neighbor_type, "
                "r.type_id AS edge_type_id, "
                "coalesce(r.logical_type, r.type, type(r)) AS edge_type";

            if (spec.return_edge_refs) {
                query +=
                    ", r.uuid AS edge_uuid, "
                    + logical_uuid_expr("startNode(r)") + " AS edge_from_uuid, "
                    + logical_uuid_expr("endNode(r)") + " AS edge_to_uuid";
            }

            const json parameters{{"frontier", frontier_json}};

            json response;
            {
                CPGDB_PROFILE_SCOPE("Neo4J fast BFS: send_req grouped label query");
                response = send_req(query, parameters);
            }

            {
                CPGDB_PROFILE_SCOPE("Neo4J fast BFS: parse minimal scalar rows");
                parse_fast_response_into_minimal_rows(
                    response,
                    rows,
                    row_by_source_uuid,
                    spec
                );
            }
        }

        return rows;
    }

    std::vector<AdjacencyRow>
    get_neighbors_batch_fast(std::span<const NodeRef> frontier,
                             const ExpandSpec& spec) const {
        auto minimal_rows = get_minimal_neighbors_batch_fast(frontier, spec);

        std::vector<AdjacencyRow> rich_rows;
        rich_rows.reserve(minimal_rows.size());

        for (auto& minimal : minimal_rows) {
            AdjacencyRow rich;
            rich.source = minimal.source;
            rich.neighbors.reserve(minimal.neighbors.size());

            for (const auto& neighbor : minimal.neighbors) {
                rich.neighbors.push_back(NeighborRef{
                    .vertex = neighbor,
                    .via_edge = std::nullopt
                });
            }

            rich_rows.push_back(std::move(rich));
        }

        return rich_rows;
    }

private:
    static std::string escape_cypher_identifier(const std::string& name) {
        std::string escaped;
        escaped.reserve(name.size() + 2);
        escaped.push_back('`');
        for (char c : name) {
            if (c == '`') {
                escaped += "``";
            } else {
                escaped.push_back(c);
            }
        }
        escaped.push_back('`');
        return escaped;
    }

    std::vector<std::string> edge_type_names_for_spec(const ExpandSpec& spec) const {
        std::vector<std::string> names;

        if (spec.allowed_edge_types.empty()) {
            const auto& descriptor = db_context_.backend_registry->descriptor(NEO4J);
            names.reserve(descriptor.coverage.edge_types.size());

            for (TypeId edge_type_id : descriptor.coverage.edge_types) {
                names.push_back(db_context_.schema->edge_type(edge_type_id).name);
            }
        } else {
            names.reserve(spec.allowed_edge_types.size());

            for (TypeId edge_type_id : spec.allowed_edge_types) {
                names.push_back(db_context_.schema->edge_type(edge_type_id).name);
            }
        }

        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    static std::string relationship_type_pattern(const std::vector<std::string>& edge_type_names) {
        if (edge_type_names.empty()) {
            return "";
        }

        std::string result = ":";
        for (std::size_t i = 0; i < edge_type_names.size(); ++i) {
            if (i > 0) {
                result += "|";
            }
            result += escape_cypher_identifier(edge_type_names[i]);
        }
        return result;
    }

    static std::string build_edge_match(Direction direction,
                                        const std::string& relationship_type_pattern) {
        switch (direction) {
            case Direction::Directed:
                return "MATCH (source)-[r" + relationship_type_pattern + "]->(neighbor) ";
            case Direction::Undirected:
            case Direction::Both:
                return "MATCH (source)-[r" + relationship_type_pattern + "]-(neighbor) ";
        }

        throw std::runtime_error("Unknown Direction in Neo4JQueryVisitor");
    }

    std::string physical_source_label_for_neo4j(const NodeRef& node) const {
        const std::string logical_name = db_context_.schema->node_type(node.type).name;

        if (logical_name == "Proxy") {
            return "Proxy";
        }

        const auto& neo4j_coverage =
            db_context_.backend_registry->descriptor(NEO4J).coverage;

        if (neo4j_coverage.covers_node_type(node.type)) {
            return logical_name;
        }

        return "Proxy";
    }

    std::unordered_map<std::string, std::vector<NodeRef>>
    group_frontier_by_physical_source_label(std::span<const NodeRef> frontier) const {
        std::unordered_map<std::string, std::vector<NodeRef>> grouped;
        for (const auto& source : frontier) {
            grouped[physical_source_label_for_neo4j(source)].push_back(source);
        }
        return grouped;
    }

    static bool contains_nul_byte(const std::string& value) {
        return value.find('\0') != std::string::npos;
    }

    static std::string printable_string(const std::string& value) {
        std::ostringstream oss;
        for (unsigned char c : value) {
            if (c == '\0') {
                oss << "\\0";
            } else {
                oss << static_cast<char>(c);
            }
        }
        return oss.str();
    }

    TypeId node_type_id_cached(const std::string& name) const {
        auto it = node_type_cache_.find(name);
        if (it != node_type_cache_.end()) {
            return it->second;
        }
        return db_context_.schema->node_type_id(name);
    }

    TypeId edge_type_id_cached(const std::string& name) const {
        auto it = edge_type_cache_.find(name);
        if (it != edge_type_cache_.end()) {
            return it->second;
        }
        return db_context_.schema->edge_type_id(name);
    }

    GlobalId uuid_cached(const std::string& uuid_string) const {
        auto it = uuid_cache_.find(uuid_string);
        if (it != uuid_cache_.end()) {
            return it->second;
        }

        GlobalId parsed = boost::lexical_cast<boost::uuids::uuid>(uuid_string);
        uuid_cache_.emplace(uuid_string, parsed);
        return parsed;
    }

    TypeId type_id_from_json_or_name(const json& id_value,
                                     const json& name_value,
                                     bool node_type) const {
        if (id_value.is_number_unsigned()) {
            auto raw = id_value.get<std::uint64_t>();
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<TypeId>::max())) {
                throw std::runtime_error("Type id is too large: " + std::to_string(raw));
            }
            return static_cast<TypeId>(raw);
        }

        if (id_value.is_number_integer()) {
            auto raw = id_value.get<std::int64_t>();
            if (raw < 0 || raw > static_cast<std::int64_t>(std::numeric_limits<TypeId>::max())) {
                throw std::runtime_error("Invalid signed type id: " + std::to_string(raw));
            }
            return static_cast<TypeId>(raw);
        }

        if (!name_value.is_string()) {
            throw std::runtime_error("Missing logical type name in Neo4J fast response");
        }

        std::string name = name_value.get<std::string>();
        if (name.empty() || contains_nul_byte(name)) {
            throw std::runtime_error(
                "Invalid logical type name from Neo4J fast response: '" +
                printable_string(name) + "'"
            );
        }

        return node_type ? node_type_id_cached(name) : edge_type_id_cached(name);
    }

    void parse_fast_response_into_minimal_rows(
        const json& response,
        std::vector<MinimalAdjacencyRow>& rows,
        const std::unordered_map<std::string, std::size_t>& row_by_source_uuid,
        const ExpandSpec& spec
    ) const {
        if (response.contains("errors") && !response["errors"].empty()) {
            throw std::runtime_error(
                "Neo4J fast neighbor query failed: " + response["errors"].dump()
            );
        }

        if (!response.contains("results") ||
            response["results"].empty() ||
            !response["results"][0].contains("data")) {
            return;
        }

        const auto& data = response["results"][0]["data"];

        for (const auto& item : data) {
            if (!item.contains("row") || !item["row"].is_array()) {
                continue;
            }

            const auto& r = item["row"];

            if (r.size() < 6 || r[0].is_null() || r[1].is_null()) {
                continue;
            }

            const std::string source_uuid_str = r[0].get<std::string>();
            auto row_it = row_by_source_uuid.find(source_uuid_str);
            if (row_it == row_by_source_uuid.end()) {
                continue;
            }

            const TypeId neighbor_type = type_id_from_json_or_name(r[2], r[3], true);
            if (!spec.allows_target_type(neighbor_type)) {
                continue;
            }

            const TypeId edge_type = type_id_from_json_or_name(r[4], r[5], false);
            if (!spec.allows_edge_type(edge_type)) {
                continue;
            }

            const std::string neighbor_uuid_str = r[1].get<std::string>();
            NodeRef neighbor_ref{
                .global_id = uuid_cached(neighbor_uuid_str),
                .type = neighbor_type
            };

            rows[row_it->second].neighbors.push_back(neighbor_ref);
        }
    }

    json send_req(const std::string& query,
                  const json& parameters) const {
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

        std::string response_body;
        {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: HTTP POST");
            response_body = http_.post(config_.endpoint, request_body);
        }

        json parsed;
        {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: parse response JSON");
            parsed = json::parse(response_body);
        }

        return parsed;
    }

    [[nodiscard]] query_result_t
    evaluate(const std::string& query,
             PARSE_TYPE parse_type,
             const json& parameters) const {
        CPGDB_PROFILE_FUNCTION();

        json response;
        {
            CPGDB_PROFILE_SCOPE("Neo4J evaluate: send_req");
            response = send_req(query, parameters);
        }

        {
            CPGDB_PROFILE_SCOPE("Neo4J evaluate: response_processor.process");
            return response_processor_.process(response, parse_type);
        }
    }
};

#endif // NEO4J_QUERY_VISITOR_HPP
