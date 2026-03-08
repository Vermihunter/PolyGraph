//
// Created by akos on 7. 3. 2026.
//

#ifndef CPGDB_LOCALLYDEPLOYEDNEO4JRESPONSEPROCESSOR_HPP
#define CPGDB_LOCALLYDEPLOYEDNEO4JRESPONSEPROCESSOR_HPP

#include "Neo4JResponseProcessor.hpp"

class LocallyDeployedNeo4JResponseProcessor : public Neo4JResponseProcessor {


public:
    query_result_t process(const json & response, PARSE_TYPE parse_type) override {
        auto& match_result = response["results"][0]; // sending only a single request

        return construct_result(match_result["data"], parse_type);
    }

private:
    static PARSE_TYPE get_parse_type(const json& json_column_type) {
        const auto column_type = json_column_type.get<std::string>();
        if (column_type == "relationship") return PARSE_TYPE::RELATIONSHIP;
        if (column_type == "node") return PARSE_TYPE::NODE;

        throw std::invalid_argument{"unknown parse type"};
    }

    static query_result_t construct_result(const json& json_data, PARSE_TYPE parse_type) {
        auto result = std::make_unique<QueryResult>();

        for (const auto& data : json_data) {
            const auto& row = data["row"][0];
            auto record = std::make_unique<Record>();

            switch (parse_type) {
                case PARSE_TYPE::NODE: {
                    record->fields.emplace_back(json_to_node(row));
                    break;
                }
                case PARSE_TYPE::RELATIONSHIP: {
                    record->fields.emplace_back(json_to_edge(row));
                    break;
                }

                default: {
                    throw std::invalid_argument("unknown parse type");
                }
            }

            result->data.push_back(std::move(record));
        }

        return result;
    }

    static Edge json_to_edge(const json& row) {
        return Edge{
            .id = row["id"],
            .type = row["type"],
            .src = row["start"],
            .dst = row["end"],
            .properties = row["properties"],
        };
    }

    static Node json_to_node(const json& row) {
        return Node{
            .id = row["id"],
            .labels = row["labels"],
            .properties = row["properties"]
        };
    }
};

#endif //CPGDB_LOCALLYDEPLOYEDNEO4JRESPONSEPROCESSOR_HPP