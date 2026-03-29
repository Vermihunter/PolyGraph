#ifndef ARANGO_QUERY_VISITOR_HPP
#define ARANGO_QUERY_VISITOR_HPP

#include <sstream>

#include <boost/lexical_cast.hpp>

#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>
#include <utilities/uuid.hpp>
#include <serialization/DataModelJsonSerializer.hpp>
#include <serialization/RecordJsonSerializer.hpp>
#include <data_model/Record.hpp>

#include "query/query_types/GetNeighborsForVertexQuery.hpp"
#include "query/query_types/GetVertexByIdQuery.hpp"

// Arango -> cuGraph
// https://docs.arango.ai/ecosystem/adapters/arangodb-cugraph-adapter/
class ArangoQueryVisitor : public QueryVisitor {
    //const std::string DATA_ENDPOINT = "http://localhost:8529/_api/cursor";

    const std::string DATA_ENDPOINT = "http://localhost:8529/_db/university/_api/cursor";
    const std::string COLLECTIONS_ENDPOINT = "http://localhost:8529/_api/collection";

    const std::string credentials = "root:rootpassword";

    DataModelJsonSerializer serializer_;


    enum class collection_t : size_t {
        NODE = 2,
        EDGE = 3
    };

public:
    ArangoQueryVisitor(DatabaseContext& db_context)
        : QueryVisitor(db_context) {}

    query_result_t visit(const GetAllEdgesQuery& q) override {
        return evaluate(construct_all_query(collection_t::EDGE), {});
    }


    query_result_t visit(const GetAllVerticesQuery& q) override {
        return evaluate(construct_all_query(collection_t::NODE), {});
    }

    query_result_t visit(const GetNeighborsForVertexQuery& q) override {
        auto& node = q.getNode();

        std::string type;

        auto it = node.properties.find("is_proxy");
        if (it != node.properties.end() && std::get<bool>(it->second->data)) {
            type = std::get<std::string>(node.properties["remote-type"]->data);
        } else {
            type = std::get<std::string>(node.properties["type"]->data);
        }

        json bindVars = {
            {"@collection", type},
            {"uuid", boost::uuids::to_string(node.global_id)}
        };

        int edgeCollectionID = 1;
        std::string queryEdges {};
        for (auto&& edge : db_context_.domain->get_edges()) {
            if (edge.from.type == type || edge.to.type == type) {
                const std::string queryAddOn = "@edges" + std::to_string(edgeCollectionID);
                bindVars[queryAddOn] = edge.type;

                if (edgeCollectionID > 1) queryEdges += ", ";
                queryEdges += ("@" + queryAddOn);
                ++edgeCollectionID;
            }
        }

        const std::string query =
            "FOR start IN @@collection\n"
                "   FILTER start.uuid == @uuid\n"
                "   FOR neighbor IN 1..1 ANY start " + queryEdges + "\n"
                "       RETURN neighbor";

        return evaluate(query, bindVars);
    }

    query_result_t visit(const GetVertexByIdQuery& q) override {
        const std::string query =
            "FOR doc IN myCollection\n"
                "FILTER doc.uuid == @uuid\n"
                "LIMIT 1\n"
                "RETURN doc\n";

        auto remote_type = std::get<std::string>(q.getNode().properties["remote-type"]->data);

        json bindVars = {
            {"bindVars", {
                {"@collection", remote_type},
                {"uuid", q.getNode().global_id}
            }}
        };

        return evaluate(query, bindVars);
    }

private:

    [[nodiscard]] json send_req(const std::string& query, const std::string& url, const json& bindVars) const {
        json req {
            {"query", query},
            { "bindVars", bindVars}
        };

        std::string res = http_post(
            url,
            req.dump(),
            credentials
        );

        return json::parse(res);
    }

    // type == 2 -> vertices
    // type == 3 -> edges
    static std::vector<std::string> fetchAndSortCollections(
        const std::string& url, 
        const std::string& auth, 
        collection_t collection_type
    ) {
        std::string raw_response = http_get(url, auth);
        std::vector<std::string> collections;

        try {
            auto data = json::parse(raw_response);

            // ArangoDB returns an array in the "result" field
            for (const auto& col : data["result"]) {
                bool isSystem = col.value("isSystem", false);
                collection_t type = collection_t(col.value("type", static_cast<size_t>(collection_t::NODE)));
                std::string name = col.value("name", "");

                // Skip internal ArangoDB collections (starting with _)
                if (isSystem || name.empty() || name[0] == '_') {
                    continue;
                }

                if (type == collection_type) {
                    collections.push_back(name);
                }
            }

        } catch (json::parse_error& e) {
            std::cerr << "JSON Parse Error: " << e.what() << std::endl;
        }

        return collections;
    }

    static std::string get_union_query_for_collections(const std::vector<std::string>& collectionNames) {
        std::stringstream union_query;
        union_query << "RETURN UNION(";
        for (size_t i = 0; i < collectionNames.size(); ++i) {
            union_query << "(FOR d IN " << collectionNames[i] << " RETURN d)";
            if (i < collectionNames.size() - 1) union_query << ", ";
        }
        union_query << ")";

        return union_query.str();
    }

    std::string construct_all_query(const collection_t collection_type) {
        // Query to get all collections for edge/vertex
        auto collectionNames =
            fetchAndSortCollections(COLLECTIONS_ENDPOINT, credentials, collection_type);

        // Get a union query that collects all vertices/edges for all collections
        return get_union_query_for_collections((collectionNames));
    }

    query_result_t evaluate(const std::string& query, const json& bindVars) const {

        // Send union query
        json response = send_req(query, DATA_ENDPOINT, bindVars);

        /**

        {
            "_key" : "5c3c88f3-85cb-4cea-bd2d-0ac29a53687f",
            "_id" : "Grade/5c3c88f3-85cb-4cea-bd2d-0ac29a53687f",
            "_rev" : "_lRnn7fi--_",
            "uuid" : "5c3c88f3-85cb-4cea-bd2d-0ac29a53687f",
            "type" : "Grade",
            "source" : "ArangoDB",
            "gradeId" : "GRD-3008",
            "points" : 79.4,
            "letter" : "A",
            "gradedAt" : "2023-03-13T15:15:00"
          }
          */
        // Transform JSON response to QueryResult
        auto result = std::make_unique<QueryResult>();
        for(auto && res_result : response["result"]) {
            record_t record = std::make_unique<Record>();

            Node node {
                .id = res_result["_id"],
                .global_id = boost::lexical_cast<uuid>(res_result["uuid"].get<std::string>()),
                .properties = res_result,
            };


             for (auto& [key, value] : res_result.items()) {
                 //std::cout << key << " : " << value << std::endl;
                 //node.properties[key] = serializer_.json_primitive_to_value(value);
             }
             //std::cout << "-------" << std::endl;
            record->fields.emplace_back(std::move(node));
            result->data.push_back(std::move(record));
        }

        return result;
    }
};


#endif // ARANGO_QUERY_VISITOR_HPP