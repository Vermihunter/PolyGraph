#ifndef NEO4J_QUERY_VISITOR_HPP
#define NEO4J_QUERY_VISITOR_HPP

#include <algorithm>
#include <optional>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/container_hash/hash.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <data_access/adapters/Neo4J/Neo4JConfig.hpp>
#include <data_access/adapters/Neo4J/Neo4JBoltClient.hpp>
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
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "response_processors/LocallyDeployedNeo4JResponseProcessor.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/Profiling.hpp"

class Neo4JQueryVisitor : public QueryVisitor {
    const BackendConnectionConfig& config_;
    LocallyDeployedNeo4JResponseProcessor response_processor_;
    bool use_bolt_ = true;
    std::string bolt_endpoint_;
    mutable std::mutex bolt_mutex_;
    mutable std::unique_ptr<Neo4JBoltClient> bolt_client_;

    const std::string getAllEdgesStrQuery =
        "MATCH ()-[r]->()\n"
        "RETURN {\n"
        "    id: elementId(r),\n"
        "    type: type(r),\n"
        "    uuid: r.uuid,\n"
        "    start: startNode(r).uuid,\n"
        "    end: endNode(r).uuid,\n"
        "    properties: properties(r)\n"
        "} AS relationship;";

    const std::string getAllVerticesStrQuery =
        "MATCH (n)\n"
        "RETURN {\n"
        "   id: elementId(n),\n"
        "   uuid: n.uuid,\n"
        "   type: coalesce(\n"
        "       CASE WHEN coalesce(n.is_proxy, false) THEN n.`remote-type` ELSE null END,\n"
        "       head([label IN labels(n) WHERE label <> 'Proxy' AND label <> 'CPGDBVertex']),\n"
        "       n.type\n"
        "   ),\n"
        "   labels: labels(n),\n"
        "   properties: properties(n)\n"
        "} AS node;";

    const std::string getVertexByIdStrQuery =
        "MATCH (n)\n"
        "WHERE n.uuid = $globalNodeId\n"
        "RETURN {\n"
        "   id: elementId(n),\n"
        "   uuid: coalesce(n.`remote-uuid`, n.uuid),\n"
        "   type: coalesce(\n"
        "       CASE WHEN coalesce(n.is_proxy, false) THEN n.`remote-type` ELSE null END,\n"
        "       head([label IN labels(n) WHERE label <> 'Proxy' AND label <> 'CPGDBVertex']),\n"
        "       n.type\n"
        "   ),\n"
        "   labels: labels(n),\n"
        "   properties: properties(n)\n"
        "} AS node;";

    const std::string getNeighborsForVertexStrQuery =
        "MATCH (n)\n"
        "WHERE n.uuid = $globalNodeId\n"
        "MATCH (n)-[]-(neighbor)\n"
        "RETURN {\n"
        "   id: elementId(neighbor),\n"
        "   uuid: coalesce(neighbor.`remote-uuid`, neighbor.uuid),\n"
        "   type: coalesce(\n"
        "       CASE WHEN coalesce(neighbor.is_proxy, false) THEN neighbor.`remote-type` ELSE null END,\n"
        "       head([label IN labels(neighbor) WHERE label <> 'Proxy' AND label <> 'CPGDBVertex']),\n"
        "       neighbor.type\n"
        "   ),\n"
        "   labels: labels(neighbor),\n"
        "   properties: properties(neighbor)\n"
        "} AS node;";

public:
    Neo4JQueryVisitor(DatabaseContext& db_context,
                      const BackendConnectionConfig& config)
        : QueryVisitor(db_context),
          config_(config),
          response_processor_(db_context),
          use_bolt_(should_use_bolt(config)),
          bolt_endpoint_(resolve_bolt_endpoint(config)) {}

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

        const std::string source_type_expr =
            "coalesce("
            "CASE WHEN coalesce(source.is_proxy, false) "
            "THEN source.`remote-type` ELSE null END, "
            "head([label IN labels(source) "
            "WHERE label <> 'Proxy' AND label <> 'CPGDBVertex']), "
            "source.type"
            ")";

        const std::string neighbor_type_expr =
            "coalesce("
            "CASE WHEN coalesce(neighbor.is_proxy, false) "
            "THEN neighbor.`remote-type` ELSE null END, "
            "head([label IN labels(neighbor) "
            "WHERE label <> 'Proxy' AND label <> 'CPGDBVertex']), "
            "neighbor.type"
            ")";

        const std::string query =
            "UNWIND $frontier AS source_uuid "
            "MATCH (source {uuid: source_uuid}) "
            + edge_match +
            "RETURN "
            "{ "
            "   id: elementId(source), "
            "   uuid: coalesce(source.`remote-uuid`, source.uuid), "
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

    /*
     * Previous get_neighbors_batch_fast implementation.
     *
     * It used direct one-hop Cypher expansion:
     *     UNWIND $frontier AS source_uuid
     *     MATCH (source:<label> {uuid: source_uuid})
     *     MATCH (source)-[r]-(neighbor)
     *
     * Kept here commented out while the active implementation below uses
     * APOC built-in BFS with minLevel=maxLevel=1.
     *
        std::vector<AdjacencyRow>
        get_neighbors_batch_fast(std::span<const NodeRef> frontier,
                                 const ExpandSpec& spec) const {
            CPGDB_PROFILE_FUNCTION();

            if (frontier.empty()) {
                return {};
            }

            std::vector<AdjacencyRow> rows;
            rows.reserve(frontier.size());

            std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>> row_by_source;
            row_by_source.reserve(frontier.size());

            for (const auto& source : frontier) {
                auto [it, inserted] = row_by_source.emplace(source.global_id, rows.size());

                if (inserted) {
                    rows.push_back(AdjacencyRow{
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

                const std::string edge_match =
                    build_edge_match(spec.direction, type_pattern);

                const std::string query =
                    "UNWIND $frontier AS source_uuid "
                    "MATCH (source:" + escape_cypher_identifier(source_label) + " {uuid: source_uuid}) "
                    + edge_match +
                    "RETURN "
                    "source.uuid AS source_uuid, "
                    "neighbor.uuid AS neighbor_uuid, "
                    "neighbor.`remote-uuid` AS neighbor_remote_uuid, "
                    "coalesce(neighbor.is_proxy, false) AS neighbor_is_proxy, "
                    "neighbor.`remote-type` AS neighbor_remote_type, "
                    "neighbor.type AS neighbor_type_property, "
                    "labels(neighbor) AS neighbor_labels, "
                    "r.uuid AS edge_uuid, "
                    "type(r) AS edge_type, "
                    "startNode(r).uuid AS edge_from_uuid, "
                    "endNode(r).uuid AS edge_to_uuid";

                const json parameters{
                    {"frontier", frontier_json}
                };

                json response;

                {
                    CPGDB_PROFILE_SCOPE("Neo4J fast BFS: send_req grouped label query");
                    response = send_req(query, parameters);
                }

                {
                    CPGDB_PROFILE_SCOPE("Neo4J fast BFS: parse scalar adjacency rows");
                    parse_fast_response_into_rows(response, rows, row_by_source, spec);
                }
            }

            return rows;
        }
     */

    std::vector<AdjacencyRow>
    get_neighbors_batch_fast(std::span<const NodeRef> frontier,
                             const ExpandSpec& spec) const {
        CPGDB_PROFILE_FUNCTION();

        if (frontier.empty()) {
            return {};
        }

        std::vector<AdjacencyRow> rows;
        rows.reserve(frontier.size());

        std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>> row_by_source;
        row_by_source.reserve(frontier.size());

        for (const auto& source : frontier) {
            auto [it, inserted] = row_by_source.emplace(source.global_id, rows.size());

            if (inserted) {
                rows.push_back(AdjacencyRow{
                    .source = source,
                    .neighbors = {}
                });
            }
        }

        const auto grouped_frontier = group_frontier_by_physical_source_label(frontier);
        const auto edge_type_names = edge_type_names_for_spec(spec);
        const std::string relationship_filter =
            apoc_relationship_filter_for_spec(edge_type_names, spec.direction);

        for (const auto& [source_label, group] : grouped_frontier) {
            if (group.empty()) {
                continue;
            }

            json frontier_json = json::array();
            for (const auto& source : group) {
                frontier_json.push_back(boost::uuids::to_string(source.global_id));
            }

            const std::string query =
                "UNWIND $frontier AS source_uuid "
                "MATCH (source:" + escape_cypher_identifier(source_label) + " {uuid: source_uuid}) "
                "CALL apoc.path.expandConfig(source, { "
                "  minLevel: 1, "
                "  maxLevel: 1, "
                "  relationshipFilter: $relationshipFilter, "
                "  bfs: true, "
                "  uniqueness: 'NODE_GLOBAL' "
                "}) YIELD path "
                "WITH source, last(nodes(path)) AS neighbor, relationships(path)[0] AS r "
                "RETURN "
                "source.uuid AS source_uuid, "
                "neighbor.uuid AS neighbor_uuid, "
                "neighbor.`remote-uuid` AS neighbor_remote_uuid, "
                "coalesce(neighbor.is_proxy, false) AS neighbor_is_proxy, "
                "neighbor.`remote-type` AS neighbor_remote_type, "
                "neighbor.type AS neighbor_type_property, "
                "labels(neighbor) AS neighbor_labels, "
                "r.uuid AS edge_uuid, "
                "type(r) AS edge_type, "
                "startNode(r).uuid AS edge_from_uuid, "
                "endNode(r).uuid AS edge_to_uuid";

            const json parameters{
                {"frontier", frontier_json},
                {"relationshipFilter", relationship_filter}
            };

            json response;

            {
                CPGDB_PROFILE_SCOPE("Neo4J fast BFS: send_req grouped APOC BFS length 1 query");
                response = send_req(query, parameters);
            }

            {
                CPGDB_PROFILE_SCOPE("Neo4J fast BFS: parse scalar adjacency rows");
                parse_fast_response_into_rows(response, rows, row_by_source, spec);
            }
        }

        return rows;
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

    std::vector<std::string>
    edge_type_names_for_spec(const ExpandSpec& spec) const {
        std::vector<std::string> names;

        if (spec.allowed_edge_types.empty()) {
            const auto& descriptor =
                db_context_.backend_registry->descriptor(NEO4J);

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

    static std::string
    relationship_type_pattern(const std::vector<std::string>& edge_type_names) {
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

    static std::string
    apoc_relationship_filter_for_spec(const std::vector<std::string>& edge_type_names,
                                      Direction direction) {
        std::string result;

        for (std::size_t i = 0; i < edge_type_names.size(); ++i) {
            const auto& edge_type_name = edge_type_names[i];

            if (edge_type_name.empty()) {
                continue;
            }

            if (edge_type_name.find('|') != std::string::npos ||
                edge_type_name.find('<') != std::string::npos ||
                edge_type_name.find('>') != std::string::npos) {
                throw std::runtime_error(
                    "Neo4J APOC relationshipFilter cannot safely encode edge type name: " +
                    edge_type_name
                );
            }

            if (!result.empty()) {
                result += "|";
            }

            switch (direction) {
                case Direction::Directed:
                    // Same semantics as build_edge_match(Direction::Directed):
                    // MATCH (source)-[r:TYPE]->(neighbor)
                    result += edge_type_name + ">";
                    break;

                case Direction::Undirected:
                case Direction::Both:
                    // Same semantics as build_edge_match(Direction::Both/Undirected):
                    // MATCH (source)-[r:TYPE]-(neighbor)
                    result += edge_type_name;
                    break;
            }
        }

        return result;
    }

    static std::string
    build_edge_match(Direction direction,
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
        const std::string logical_name =
            db_context_.schema->node_type(node.type).name;

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

    static std::optional<std::string>
    non_empty_string_opt(const json& value) {
        if (!value.is_string()) {
            return std::nullopt;
        }

        auto result = value.get<std::string>();

        if (result.empty()) {
            return std::nullopt;
        }

        return result;
    }

    static bool bool_or_false(const json& value) {
        return value.is_boolean() && value.get<bool>();
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

    static bool labels_contain(const json& labels_value,
                               const std::string& wanted) {
        if (!labels_value.is_array()) {
            return false;
        }

        for (const auto& label_value : labels_value) {
            if (label_value.is_string() &&
                label_value.get<std::string>() == wanted) {
                return true;
            }
        }

        return false;
    }

    static bool is_infrastructure_label(const std::string& label) {
        return label == "Proxy" ||
               label == "CPGDBVertex" ||
               label == "__GraphNode";
    }

    static std::optional<std::string>
    first_logical_label_opt(const json& labels_value) {
        if (!labels_value.is_array()) {
            return std::nullopt;
        }

        for (const auto& label_value : labels_value) {
            if (!label_value.is_string()) {
                continue;
            }

            std::string label = label_value.get<std::string>();

            if (label.empty()) {
                continue;
            }

            if (is_infrastructure_label(label)) {
                continue;
            }

            return label;
        }

        return std::nullopt;
    }

    static std::string
    resolve_logical_neighbor_uuid(const std::string& physical_uuid,
                                  const json& remote_uuid_value,
                                  bool looks_like_proxy) {
        if (looks_like_proxy) {
            auto remote_uuid = non_empty_string_opt(remote_uuid_value);

            if (remote_uuid.has_value()) {
                return *remote_uuid;
            }
        }

        return physical_uuid;
    }

    static std::string
    resolve_logical_neighbor_type(const json& is_proxy_value,
                                  const json& remote_type_value,
                                  const json& type_property_value,
                                  const json& labels_value,
                                  const std::string& neighbor_uuid) {
        const bool is_proxy_flag = bool_or_false(is_proxy_value);

        const auto remote_type = non_empty_string_opt(remote_type_value);
        const auto type_property = non_empty_string_opt(type_property_value);
        const auto logical_label = first_logical_label_opt(labels_value);

        const bool looks_like_proxy =
            is_proxy_flag ||
            labels_contain(labels_value, "Proxy") ||
            (type_property.has_value() && *type_property == "Proxy");

        if (looks_like_proxy) {
            if (!remote_type.has_value()) {
                throw std::runtime_error(
                    "Neo4J proxy node is missing remote-type. neighbor_uuid=" +
                    neighbor_uuid +
                    ", is_proxy=" + is_proxy_value.dump() +
                    ", remote_type=" + remote_type_value.dump() +
                    ", type_property=" + type_property_value.dump() +
                    ", labels=" + labels_value.dump()
                );
            }

            return *remote_type;
        }

        if (logical_label.has_value()) {
            return *logical_label;
        }

        if (type_property.has_value() && *type_property != "Proxy") {
            return *type_property;
        }

        throw std::runtime_error(
            "Neo4J fast path could not resolve logical type for neighbor uuid=" +
            neighbor_uuid +
            ", remote_type=" + remote_type_value.dump() +
            ", type_property=" + type_property_value.dump() +
            ", labels=" + labels_value.dump()
        );
    }

    void parse_fast_response_into_rows(
        const json& response,
        std::vector<AdjacencyRow>& rows,
        const std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>>& row_by_source,
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

            if (r.size() < 11 || r[0].is_null() || r[1].is_null()) {
                continue;
            }

            const std::string source_uuid_str = r[0].get<std::string>();
            const std::string physical_neighbor_uuid_str = r[1].get<std::string>();

            const bool looks_like_proxy =
                bool_or_false(r[3]) ||
                non_empty_string_opt(r[5]).value_or("") == "Proxy" ||
                labels_contain(r[6], "Proxy");

            const std::string neighbor_uuid_str =
                resolve_logical_neighbor_uuid(
                    physical_neighbor_uuid_str,
                    r[2],
                    looks_like_proxy
                );

            const std::string neighbor_type_name =
                resolve_logical_neighbor_type(
                    r[3],
                    r[4],
                    r[5],
                    r[6],
                    physical_neighbor_uuid_str
                );

            if (contains_nul_byte(neighbor_type_name)) {
                throw std::runtime_error(
                    "Neo4J fast path resolved invalid logical type containing NUL bytes "
                    "for neighbor uuid=" + physical_neighbor_uuid_str +
                    ": '" + printable_string(neighbor_type_name) + "'"
                );
            }

            const std::string edge_uuid_str =
                r[7].is_null() ? "" : r[7].get<std::string>();

            const std::string edge_type_name =
                r[8].get<std::string>();

            const std::string edge_from_uuid_str =
                r[9].get<std::string>();

            const std::string edge_to_uuid_str =
                r[10].get<std::string>();

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
    }

    json send_req(const std::string& query,
                  const json& parameters) const {
        CPGDB_PROFILE_FUNCTION();

        if (use_bolt_) {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: Bolt RUN/PULL");
            return send_req_bolt(query, parameters);
        }

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
        std::string auth;

        if (config_.credentials.has_value()) {
            auth = config_.credentials->username + ":" +
                   config_.credentials->password;
        }

        {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: HTTP POST");
            response_body = http_post(config_.endpoint, request_body, auth);
        }

        json parsed;

        {
            CPGDB_PROFILE_SCOPE("Neo4J send_req: parse response JSON");
            parsed = json::parse(response_body);
        }

        return parsed;
    }


    static bool should_use_bolt(const BackendConnectionConfig& config) {
        if (const char* env = std::getenv("CPGDB_NEO4J_TRANSPORT")) {
            return std::string(env) == "bolt";
        }

        return config.transport == "bolt";
    }

    static std::string resolve_bolt_endpoint(const BackendConnectionConfig& config) {
        if (!config.bolt_endpoint.empty()) {
            return config.bolt_endpoint;
        }

        // Reasonable local default for docker-compose Neo4J.
        // Keep HTTP endpoint unchanged so users can switch transport via env var.
        return "bolt://localhost:7687";
    }

    json send_req_bolt(const std::string& query,
                       const json& parameters) const {
        std::lock_guard<std::mutex> lock(bolt_mutex_);

        if (!bolt_client_) {
            std::string username;
            std::string password;

            if (config_.credentials.has_value()) {
                username = config_.credentials->username;
                password = config_.credentials->password;
            }

            bolt_client_ = std::make_unique<Neo4JBoltClient>(
                bolt_endpoint_,
                username,
                password,
                config_.database_name
            );
        }

        return bolt_client_->run(query, parameters);
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