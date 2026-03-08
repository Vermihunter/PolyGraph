#ifndef ARANGO_QUERY_VISITOR_HPP
#define ARANGO_QUERY_VISITOR_HPP

#include <sstream>

#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <query/query_types/GetAllVerticesQuery.hpp>
#include <query/query_types/GetAllEdgesQuery.hpp>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>

#include <serialization/DataModelJsonSerializer.hpp>


class ArangoQueryVisitor : public QueryVisitor {
    const std::string DATA_ENDPOINT = "http://localhost:8529/_api/cursor";
    const std::string COLLECTIONS_ENDPOINT = "http://localhost:8529/_api/collection";

    const std::string credentials = "root:rootpassword";

    DataModelJsonSerializer serializer_;
public:
    virtual query_result_t visit(const GetAllEdgesQuery& q) {
        return evaluate(3);
    }


    virtual query_result_t visit(const GetAllVerticesQuery& q) {
        return evaluate(2);
    }

private:

    json send_req(const std::string& query, const std::string& url) const {
        json req {
            {"query", query}
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
    static std::vector<std::string> fetchAndSortCollections(const std::string& url, const std::string& auth, int collectionType) {
        std::string rawResponse = http_get(url, auth);
        std::vector<std::string> collections;

        try {
            auto data = json::parse(rawResponse);

            // ArangoDB returns an array in the "result" field
            for (const auto& col : data["result"]) {
                bool isSystem = col.value("isSystem", false);
                int type = col.value("type", 2);
                std::string name = col.value("name", "");

                // Skip internal ArangoDB collections (starting with _)
                if (isSystem || name.empty() || name[0] == '_') {
                    continue;
                }

                if (type == collectionType) {
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

    query_result_t evaluate(const int collectionType) {
        // Query to get all collections for edge/vertex
        auto collectionNames =
            fetchAndSortCollections(COLLECTIONS_ENDPOINT, credentials, collectionType);

        // Get a union query that collects all vertices/edges for all collections
        auto union_query = get_union_query_for_collections((collectionNames));

        // Send union query
        json response = send_req(union_query, DATA_ENDPOINT);

        // Transform JSON response to QueryResult
        auto result = std::make_unique<QueryResult>();
        for(auto && res_result : response["result"][0]) {
            record_t record = std::make_unique<Record>();
            Node node {
                .id = res_result["_id"],
            };

             for (auto& [key, value] : res_result.items()) {
                 //std::cout << key << " : " << value << std::endl;
                 node.properties[key] = serializer_.json_primitive_to_value(value);
             }
             //std::cout << "-------" << std::endl;
            record->fields.emplace_back(std::move(node));
            result->data.push_back(std::move(record));
        }

        return result;
    }
};


#endif // ARANGO_QUERY_VISITOR_HPP