#ifndef ARANGO_QUERY_VISITOR_HPP
#define ARANGO_QUERY_VISITOR_HPP

#include <optional>
#include <sstream>
#include <unordered_map>

#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>
#include <utilities/uuid.hpp>
#include <serialization/DataModelJsonSerializer.hpp>
#include <serialization/RecordJsonSerializer.hpp>
#include <data_model/Record.hpp>

#include "data_access/adapters/Neo4J/response_processors/Neo4JResponseProcessor.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "query/query_types/GetNeighborsBatchQuery.hpp"
#include "query/query_types/GetNeighborsForVertexQuery.hpp"
#include "query/query_types/GetVertexByIdQuery.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/logging.hpp"
#include "utilities/Profiling.hpp"

class ArangoQueryVisitor : public QueryVisitor {
    const BackendConnectionConfig& config_;
    DataModelJsonSerializer serializer_;
    mutable CurlHttpClient http_;

    enum class collection_t : std::size_t {
        NODE = 2,
        EDGE = 3
    };

public:
    ArangoQueryVisitor(DatabaseContext& db_context,
                       const BackendConnectionConfig& config)
        : QueryVisitor(db_context),
          config_(config),
          http_(config.credentials.has_value()
                    ? config.credentials->username + ":" + config.credentials->password
                    : "") {}

    query_result_t visit(const GetAllEdgesQuery& q) override {
        return evaluate(
            construct_all_query(collection_t::EDGE),
            json::object(),
            PARSE_TYPE::RELATIONSHIP
        );
    }

    query_result_t visit(const GetAllVerticesQuery& q) override {
        return evaluate(
            construct_all_query(collection_t::NODE),
            json::object(),
            PARSE_TYPE::NODE
        );
    }

    query_result_t visit(const GetNeighborsForVertexQuery& q) override {
        const Node& node = q.getNode();

        NodeRef ref{
            .global_id = node.global_id,
            .type = node.type
        };

        std::vector<NodeRef> frontier;
        frontier.push_back(ref);

        ExpandSpec spec;
        spec.direction = Direction::Both;

        return visit(GetNeighborsBatchQuery(std::move(frontier), spec));
    }

    query_result_t visit(const GetVertexByIdQuery& q) override {
        const Node& node = q.getNode();

        const std::string collection =
            db_context_.schema->node_type(node.type).name;

        const std::string query =
            "FOR doc IN @@collection "
            "FILTER doc.uuid == @uuid "
            "LIMIT 1 "
            "RETURN doc";

        json bindVars = {
            {"@collection", collection},
            {"uuid", boost::uuids::to_string(node.global_id)}
        };

        return evaluate(query, bindVars, PARSE_TYPE::NODE);
    }

    query_result_t visit(const GetNeighborsBatchQuery& q) override {
        //CPGDB_PROFILE_FUNCTION();

        EVENT_SCOPE(_, "arango.neighbor_query", {"frontier_size", q.frontier().size()});
        auto grouped_frontier = group_frontier_by_collection(q.frontier());

        auto combined = std::make_unique<QueryResult>();

        for (const auto& [collection, frontier] : grouped_frontier) {
            if (frontier.empty()) {
                continue;
            }

            json frontierJson = json::array();

            for (const auto& v : frontier) {
                frontierJson.push_back(boost::uuids::to_string(v.global_id));
            }

            std::vector<TypeId> edge_type_ids = edge_types_for_query(q.spec());

            if (edge_type_ids.empty()) {
                continue;
            }

            json bindVars = {
                {"@vertexCollection", collection},
                {"frontier", frontierJson}
            };

            std::string edgeCollectionsAql;

            for (std::size_t i = 0; i < edge_type_ids.size(); ++i) {
                const std::string bindName = "edgeCollection" + std::to_string(i);
                const std::string bindKey = "@" + bindName;

                bindVars[bindKey] =
                    db_context_.schema->edge_type(edge_type_ids[i]).name;

                if (i > 0) {
                    edgeCollectionsAql += ", ";
                }

                edgeCollectionsAql += "@@" + bindName;
            }

            const std::string direction = arango_direction(q.spec().direction);

            /*
             * Result shape:
             *
             * {
             *   source: start,
             *   neighbor: neighbor,
             *   edge: MERGE(edge, {
             *      start: DOCUMENT(edge._from).uuid,
             *      end: DOCUMENT(edge._to).uuid
             *   })
             * }
             *
             * DOCUMENT(edge._from).uuid and DOCUMENT(edge._to).uuid preserve
             * the real stored edge direction even when traversal uses ANY.
             */
            const std::string query =
                "FOR start IN @@vertexCollection "
                "FILTER start.uuid IN @frontier "
                "FOR neighbor, edge IN 1..1 " + direction + " start " + edgeCollectionsAql + " "
                "OPTIONS { uniqueVertices: \"none\" } "
                "LET edgeStart = DOCUMENT(edge._from) "
                "LET edgeEnd = DOCUMENT(edge._to) "
                "RETURN { "
                "  source: start, "
                "  neighbor: neighbor, "
                "  edge: MERGE(edge, { "
                "    start: edgeStart.uuid, "
                "    end: edgeEnd.uuid "
                "  }) "
                "}";

            auto partial = evaluate(query, bindVars, PARSE_TYPE::ADJACENCY_ROW);
            combined->combine(std::move(partial));
        }

        return combined;
    }

private:
    std::unordered_map<std::string, std::vector<NodeRef>>
    group_frontier_by_collection(const std::vector<NodeRef>& frontier) const {
        std::unordered_map<std::string, std::vector<NodeRef>> grouped;

        for (const auto& node : frontier) {
            grouped[physical_vertex_collection_for(node)].push_back(node);
        }

        return grouped;
    }

    [[nodiscard]] std::string
    physical_vertex_collection_for(const NodeRef& node) const {
        const std::string logical_name =
            db_context_.schema->node_type(node.type).name;

        if (logical_name == "Proxy") {
            return "Proxy";
        }

        const auto& coverage =
            db_context_.backend_registry->descriptor(ARANGO).coverage;

        /*
         * Arango stores real Arango-owned nodes in collections named after
         * their logical type. Cross-backend endpoint nodes are stored in the
         * physical Proxy collection by tools/generators/from-file-to-db.py.
         *
         * Example: CourseOffering is a GPU node type. For the Arango edge
         * CourseOffering--SCHEDULED_AT--ScheduleSlot, the CourseOffering
         * endpoint exists in Arango as Proxy/<uuid>, not CourseOffering/<uuid>.
         */
        if (coverage.node_types.contains(node.type)) {
            return logical_name;
        }

        return "Proxy";
    }

    std::vector<TypeId>
    edge_types_for_query(const ExpandSpec& spec) const {
        if (!spec.allowed_edge_types.empty()) {
            return spec.allowed_edge_types;
        }

        std::vector<TypeId> result;

        const auto& coverage =
            db_context_.backend_registry->descriptor(ARANGO).coverage;

        for (TypeId edge_type_id : coverage.edge_types) {
            result.push_back(edge_type_id);
        }

        return result;
    }

    static std::string arango_direction(Direction direction) {
        switch (direction) {
            case Direction::Directed:
                return "OUTBOUND";

            case Direction::Undirected:
                return "ANY";

            case Direction::Both:
                return "ANY";
        }

        return "ANY";
    }

    [[nodiscard]] json send_req(const std::string& query,
                                const json& bindVars) const {
        json req {
            {"query", query},
            {"bindVars", bindVars},
            // Neighbor expansion can easily return thousands of rows for one
            // BFS layer. ArangoDB cursors are paginated; using a larger batch
            // reduces round-trips, and the loop below still handles hasMore.
            {"batchSize", 1000000}
        };

        std::string res = http_.post(
            config_.endpoint,
            req.dump()
        );

        json response = json::parse(res);

        if (response.contains("error") &&
            response["error"].is_boolean() &&
            response["error"].get<bool>()) {
            return response;
        }

        if (!response.contains("result") || !response["result"].is_array()) {
            return response;
        }

        json combined_result = response["result"];

        while (response.value("hasMore", false)) {
            if (!response.contains("id") || !response["id"].is_string()) {
                throw std::runtime_error(
                    "ArangoDB cursor says hasMore=true but response has no cursor id: " +
                    response.dump()
                );
            }

            const std::string next_url =
                config_.endpoint + "/" + response["id"].get<std::string>();

            std::string next_res = http_.put(next_url, "{}");
            response = json::parse(next_res);

            if (response.contains("error") &&
                response["error"].is_boolean() &&
                response["error"].get<bool>()) {
                return response;
            }

            if (!response.contains("result") || !response["result"].is_array()) {
                throw std::runtime_error(
                    "ArangoDB cursor page does not contain result array: " +
                    response.dump()
                );
            }

            for (const auto& item : response["result"]) {
                combined_result.push_back(item);
            }
        }

        response["result"] = std::move(combined_result);
        response["hasMore"] = false;
        return response;
    }

    static std::vector<std::string>
    fetchAndSortCollections(const std::string& url,
                            const std::string& auth,
                            collection_t collection_type) {
        std::string raw_response = http_get(url, auth);
        std::vector<std::string> collections;

        auto data = json::parse(raw_response);

        for (const auto& col : data["result"]) {
            bool isSystem = col.value("isSystem", false);

            collection_t type = static_cast<collection_t>(
                col.value("type", static_cast<std::size_t>(collection_t::NODE))
            );

            std::string name = col.value("name", "");

            if (isSystem || name.empty() || name[0] == '_') {
                continue;
            }

            if (type == collection_type) {
                collections.push_back(name);
            }
        }

        return collections;
    }

    static std::string
    get_union_query_for_collections(const std::vector<std::string>& collectionNames) {
        std::stringstream union_query;

        union_query << "RETURN UNION(";

        for (std::size_t i = 0; i < collectionNames.size(); ++i) {
            union_query << "(FOR d IN " << collectionNames[i] << " RETURN d)";

            if (i + 1 < collectionNames.size()) {
                union_query << ", ";
            }
        }

        union_query << ")";

        return union_query.str();
    }

    std::string construct_all_query(collection_t collection_type) const {
        std::string auth;

        if (config_.credentials.has_value()) {
            auth = config_.credentials->username + ":" +
                   config_.credentials->password;
        }

        /*
         * If your config endpoint is:
         *   http://localhost:8529/_db/university/_api/cursor
         *
         * Then collection endpoint should usually be:
         *   http://localhost:8529/_db/university/_api/collection
         *
         * For now, simplest option: keep it configured separately later.
         */
        const std::string collectionsEndpoint =
            "http://localhost:8529/_db/" +
            config_.database_name +
            "/_api/collection";

        auto collectionNames =
            fetchAndSortCollections(collectionsEndpoint, auth, collection_type);

        return get_union_query_for_collections(collectionNames);
    }

    query_result_t evaluate(const std::string& query,
                            const json& bindVars,
                            PARSE_TYPE parse_type) const {
        json response = send_req(query, bindVars);

        if (response.contains("error") &&
            response["error"].is_boolean() &&
            response["error"].get<bool>()) {
            throw std::runtime_error(
                "ArangoDB returned error: " + response.dump()
            );
        }

        return construct_result(response, parse_type);
    }

    query_result_t construct_result(const json& response,
                                    PARSE_TYPE parse_type) const {
        auto result = std::make_unique<QueryResult>();

        if (!response.contains("result")) {
            throw std::runtime_error(
                "ArangoDB response does not contain result: " + response.dump()
            );
        }

        auto dump = response.dump();
        for (const auto& item : response["result"]) {
            /*
             * construct_all_query returns one item which is an array.
             * Flatten it.
             */
            if (item.is_array()) {
                for (const auto& nested : item) {
                    append_record(*result, nested, parse_type);
                }
            } else {
                append_record(*result, item, parse_type);
            }
        }

        return result;
    }

    void append_record(QueryResult& result,
                       const json& item,
                       PARSE_TYPE parse_type) const {
        auto record = std::make_unique<Record>();

        switch (parse_type) {
            case PARSE_TYPE::NODE: {
                record->fields.emplace_back(json_to_node(item));
                break;
            }

            case PARSE_TYPE::RELATIONSHIP: {
                record->fields.emplace_back(json_to_edge(item));
                break;
            }

            case PARSE_TYPE::ADJACENCY_ROW: {
                /*
                 * Expected item:
                 *
                 * {
                 *   "source": {...},
                 *   "neighbor": {...},
                 *   "edge": {...}
                 * }
                 */
                if (!item.contains("source") ||
                    !item.contains("neighbor") ||
                    !item.contains("edge")) {
                    throw std::runtime_error(
                        "Arango adjacency item must contain source, neighbor, edge: "
                        + item.dump()
                    );
                }

                record->fields.emplace_back(json_to_node(item["source"]));
                record->fields.emplace_back(json_to_node(item["neighbor"]));
                record->fields.emplace_back(json_to_edge(item["edge"]));
                break;
            }

            default:
                throw std::invalid_argument("Unknown PARSE_TYPE");
        }

        result.data.push_back(std::move(record));
    }

    Node json_to_node(const json& row) const {
        if (!row.contains("uuid")) {
            throw std::runtime_error(
                "Arango node row does not contain uuid: " + row.dump()
            );
        }

        const std::string uuid_str = row["uuid"].get<std::string>();

        /*
         * Important:
         *
         * For Arango, do NOT use row["type"] as the logical node type.
         * In your data, row["type"] may be a domain property, e.g. "program".
         *
         * Logical type comes from the collection name:
         *   _id = "Application/5185ab0e-..."
         *   logical type = "Application"
         */
        std::string logical_type_name = arango_collection_from_document(row);

        if (logical_type_name == "Proxy") {
            auto remote_type = get_string_property_opt(row, "remote-type");
            if (remote_type.has_value()) {
                logical_type_name = *remote_type;
            }
        }

        return Node {
            .global_id = boost::lexical_cast<uuid>(uuid_str),
            .type = db_context_.schema->node_type_id(logical_type_name),
            .labels = {logical_type_name},
            .properties = arango_document_properties(row)
        };
    }

    static std::string arango_collection_from_document(const json& row) {
        if (row.contains("_id") && row["_id"].is_string()) {
            return collection_from_arango_id(row["_id"].get<std::string>());
        }

        /*
         * Fallback only for non-Arango-shaped projected documents.
         * Use this only if no _id exists.
         */
        if (row.contains("collection") && row["collection"].is_string()) {
            return row["collection"].get<std::string>();
        }

        throw std::runtime_error(
            "Cannot infer Arango collection/logical type from document: " + row.dump()
        );
    }

    static std::string collection_from_arango_id(const std::string& id) {
        const auto slash = id.find('/');

        if (slash == std::string::npos || slash == 0) {
            throw std::runtime_error("Invalid Arango _id: " + id);
        }

        return id.substr(0, slash);
    }

    static std::optional<std::string>
    get_string_property_opt(const json& row, const std::string& name) {
        if (row.contains(name) && row[name].is_string()) {
            return row[name].get<std::string>();
        }

        if (row.contains("properties") &&
            row["properties"].is_object() &&
            row["properties"].contains(name) &&
            row["properties"][name].is_string()) {
            return row["properties"][name].get<std::string>();
        }

        return std::nullopt;
    }

    static Map arango_document_properties(const json& row) {
        /*
         * If you ever return nested properties explicitly, support that too.
         */
        if (row.contains("properties") && row["properties"].is_object()) {
            return row["properties"].get<Map>();
        }

        /*
         * Otherwise your Arango document is flat:
         *
         * {
         *   "_id": "...",
         *   "_key": "...",
         *   "_rev": "...",
         *   "uuid": "...",
         *   "type": "program",
         *   "applicationId": "..."
         * }
         *
         * We keep user/domain fields such as:
         *   type = program
         *   applicationId = ...
         *   status = ...
         *
         * We remove Arango system fields.
         */
        json props = row;

        props.erase("_id");
        props.erase("_key");
        props.erase("_rev");

        /*
         * Optional:
         * If you do not want uuid duplicated in properties, erase it too.
         * If your current code expects properties["uuid"], leave it.
         */
        // props.erase("uuid");

        return props.get<Map>();
    }

    Edge json_to_edge(const json& row) const {
        if (!row.contains("uuid")) {
            throw std::runtime_error(
                "Arango edge row does not contain uuid: " + row.dump()
            );
        }

        const std::string uuid_str = row["uuid"].get<std::string>();

        /*
         * Same rule as nodes:
         * logical edge type comes from edge collection name, not row["type"].
         */
        const std::string logical_edge_type_name =
            arango_collection_from_document(row);

        std::string start_uuid;
        std::string end_uuid;

        if (row.contains("start") && row.contains("end")) {
            start_uuid = row["start"].get<std::string>();
            end_uuid = row["end"].get<std::string>();
        } else if (row.contains("_from") && row.contains("_to")) {
            start_uuid = uuid_from_arango_document_id(row["_from"].get<std::string>());
            end_uuid = uuid_from_arango_document_id(row["_to"].get<std::string>());
        } else {
            throw std::runtime_error(
                "Arango edge row does not contain start/end or _from/_to: " +
                row.dump()
            );
        }

        return Edge {
            .global_id = boost::lexical_cast<uuid>(uuid_str),
            .type = db_context_.schema->edge_type_id(logical_edge_type_name),
            .src = boost::lexical_cast<uuid>(start_uuid),
            .dst = boost::lexical_cast<uuid>(end_uuid),
            .properties = arango_edge_properties(row)
        };
    }

    static Map arango_edge_properties(const json& row) {
        if (row.contains("properties") && row["properties"].is_object()) {
            return row["properties"].get<Map>();
        }

        json props = row;

        props.erase("_id");
        props.erase("_key");
        props.erase("_rev");
        props.erase("_from");
        props.erase("_to");

        /*
         * These are artificial fields added by your query:
         *
         * edge: MERGE(edge, {
         *   start: edgeStart.uuid,
         *   end: edgeEnd.uuid
         * })
         */
        props.erase("start");
        props.erase("end");

        // Optional:
        // props.erase("uuid");

        return props.get<Map>();
    }

    static std::string uuid_from_arango_document_id(const std::string& id) {
        /*
         * Arango _id shape:
         *
         *   CollectionName/key
         *
         * If your _key is the uuid, this works.
         */
        auto slash = id.find('/');

        if (slash == std::string::npos || slash + 1 >= id.size()) {
            throw std::runtime_error("Invalid Arango document id: " + id);
        }

        return id.substr(slash + 1);
    }
};

#endif // ARANGO_QUERY_VISITOR_HPP