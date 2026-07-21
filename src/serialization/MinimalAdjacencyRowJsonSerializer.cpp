//
// Created by akos on 11. 7. 2026.
//

#include "serialization/MinimalAdjacencyRowJsonSerializer.hpp"
#include "utilities/uuid.hpp"

#include "serialization/NodeRefJsonSerializer.hpp"


namespace cpgdb {
    void to_json(json &value, const MinimalAdjacencyRow &adj_row) {
        value = json{{"source-id", global_id_to_string(adj_row.source.global_id)}, {"neighbors", adj_row.neighbors}};
    }

    /*
    void from_json(const json& value, MinimalAdjacencyRow& node) {
        node.global_id = json_to_uuid(value.at("id"));
        node.type = value.value("type", TypeId{});
        node.labels = value.at("labels").get<std::vector<std::string>>();
        node.properties =
            value.at("properties").get<std::unordered_map<std::string, std::unique_ptr<Value>>>();
    }
    */
} // namespace cpgdb
