#ifndef CPGDB_LOCALLYDEPLOYEDNEO4JRESPONSEPROCESSOR_HPP
#define CPGDB_LOCALLYDEPLOYEDNEO4JRESPONSEPROCESSOR_HPP

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid.hpp>

#include "Neo4JResponseProcessor.hpp"
#include "serialization/RecordJsonSerializer.hpp"
#include "utilities/Profiling.hpp"

class LocallyDeployedNeo4JResponseProcessor : public Neo4JResponseProcessor {
    DatabaseContext& db_context_;

public:
    explicit LocallyDeployedNeo4JResponseProcessor(DatabaseContext& db_context)
        : db_context_(db_context) {}

    [[nodiscard]] query_result_t
    process(const json& response,
            PARSE_TYPE parse_type) const override {
        CPGDB_PROFILE_FUNCTION();

        if (response.contains("errors") && !response["errors"].empty()) {
            throw std::runtime_error(
                "Neo4j returned errors: " + response["errors"].dump()
            );
        }

        if (!response.contains("results") || response["results"].empty()) {
            throw std::runtime_error("Neo4j response does not contain results");
        }

        const auto& result_block = response["results"][0];

        if (!result_block.contains("data")) {
            throw std::runtime_error("Neo4j result block does not contain data");
        }

        return construct_result(result_block, parse_type);
    }

private:
    [[nodiscard]] query_result_t
    construct_result(const json& result_block,
                     PARSE_TYPE parse_type) const {
        CPGDB_PROFILE_FUNCTION();

        auto result = std::make_unique<QueryResult>();

        const auto& data_rows = result_block.at("data");

        for (const auto& data_entry : data_rows) {
            if (!data_entry.contains("row")) {
                throw std::runtime_error("Neo4j data entry does not contain row");
            }

            const auto& row = data_entry.at("row");

            auto record = std::make_unique<Record>();

            switch (parse_type) {
                case PARSE_TYPE::NODE: {
                    require_row_size_at_least(row, 1, "NODE");
                    record->fields.emplace_back(json_to_node(row[0]));
                    break;
                }

                case PARSE_TYPE::RELATIONSHIP: {
                    require_row_size_at_least(row, 1, "RELATIONSHIP");
                    record->fields.emplace_back(json_to_edge(row[0]));
                    break;
                }

                case PARSE_TYPE::ADJACENCY_ROW: {
                    require_row_size_at_least(row, 3, "ADJACENCY_ROW");
                    record->fields.emplace_back(json_to_node(row[0]));
                    record->fields.emplace_back(json_to_node(row[1]));
                    record->fields.emplace_back(json_to_edge(row[2]));
                    break;
                }

                default:
                    throw std::invalid_argument("Unknown PARSE_TYPE");
            }

            result->data.push_back(std::move(record));
        }

        return result;
    }

    static void require_row_size_at_least(const json& row,
                                          std::size_t expected,
                                          const std::string& parse_name) {
        if (!row.is_array()) {
            throw std::runtime_error(parse_name + " expected row to be an array");
        }

        if (row.size() < expected) {
            throw std::runtime_error(
                parse_name + " expected at least " +
                std::to_string(expected) +
                " columns, got " +
                std::to_string(row.size())
            );
        }
    }

    static std::string get_uuid_string(const json& row,
                                       const std::string& entity_name) {
        if (row.contains("uuid") && row["uuid"].is_string()) {
            return row["uuid"].get<std::string>();
        }

        if (row.contains("properties") &&
            row["properties"].is_object() &&
            row["properties"].contains("uuid") &&
            row["properties"]["uuid"].is_string()) {
            return row["properties"]["uuid"].get<std::string>();
        }

        throw std::runtime_error(entity_name + " JSON does not contain uuid: " + row.dump());
    }

    static bool get_bool_property(const json& row,
                                  const std::string& name,
                                  bool default_value = false) {
        if (row.contains(name) && row[name].is_boolean()) {
            return row[name].get<bool>();
        }

        if (row.contains("properties") &&
            row["properties"].is_object() &&
            row["properties"].contains(name) &&
            row["properties"][name].is_boolean()) {
            return row["properties"][name].get<bool>();
        }

        return default_value;
    }

    static std::optional<std::string>
    get_string_property_opt(const json& row,
                            const std::string& name) {
        if (row.contains(name) && row[name].is_string()) {
            auto result = row[name].get<std::string>();

            if (!result.empty()) {
                return result;
            }
        }

        if (row.contains("properties") &&
            row["properties"].is_object() &&
            row["properties"].contains(name) &&
            row["properties"][name].is_string()) {
            auto result = row["properties"][name].get<std::string>();

            if (!result.empty()) {
                return result;
            }
        }

        return std::nullopt;
    }

    static std::string
    get_required_string_property(const json& row,
                                 const std::string& name,
                                 const std::string& entity_name) {
        auto value = get_string_property_opt(row, name);

        if (!value.has_value()) {
            throw std::runtime_error(
                entity_name + " JSON does not contain string property: " +
                name + ". JSON: " + row.dump()
            );
        }

        return *value;
    }

    static std::vector<std::string>
    get_labels(const json& row) {
        if (row.contains("labels") && row["labels"].is_array()) {
            return row["labels"].get<std::vector<std::string>>();
        }

        return {};
    }

    static std::optional<std::string>
    first_logical_label(const std::vector<std::string>& labels) {
        for (const auto& label : labels) {
            if (label != "Proxy" && label != "CPGDBVertex") {
                return label;
            }
        }

        return std::nullopt;
    }

    static bool has_label(const std::vector<std::string>& labels,
                          const std::string& wanted) {
        return std::find(labels.begin(), labels.end(), wanted) != labels.end();
    }

    static bool contains_nul_byte(const std::string& value) {
        return value.find('\0') != std::string::npos;
    }

    static std::string printable_string(const std::string& value) {
        std::string out;
        out.reserve(value.size());

        for (unsigned char c : value) {
            if (c == '\0') {
                out += "\\0";
            } else {
                out.push_back(static_cast<char>(c));
            }
        }

        return out;
    }

    static Map get_properties(const json& row) {
        if (row.contains("properties") &&
            row["properties"].is_object()) {
            return row["properties"].get<Map>();
        }

        return row.get<Map>();
    }

    [[nodiscard]] Node json_to_node(const json& row) const {
        CPGDB_PROFILE_FUNCTION();

        std::string uuid_str =
            get_required_string_property(row, "uuid", "Neo4j node");

        const std::vector<std::string> labels = get_labels(row);

        const auto explicit_type = get_string_property_opt(row, "type");
        const auto remote_type = get_string_property_opt(row, "remote-type");
        const auto remote_uuid = get_string_property_opt(row, "remote-uuid");

        const bool is_proxy =
            get_bool_property(row, "is_proxy", false) ||
            has_label(labels, "Proxy") ||
            explicit_type.value_or("") == "Proxy";

        if (is_proxy && remote_uuid.has_value()) {
            uuid_str = *remote_uuid;
        }

        std::string type_name;

        if (is_proxy) {
            if (!remote_type.has_value()) {
                throw std::runtime_error(
                    "Neo4j proxy node does not contain remote-type: " + row.dump()
                );
            }

            type_name = *remote_type;
        } else {
            auto label_type = first_logical_label(labels);

            if (label_type.has_value()) {
                type_name = *label_type;
            } else if (explicit_type.has_value() && *explicit_type != "Proxy") {
                type_name = *explicit_type;
            } else {
                throw std::runtime_error(
                    "Neo4j node does not contain usable logical type: " + row.dump()
                );
            }
        }

        if (contains_nul_byte(type_name)) {
            throw std::runtime_error(
                "Neo4j node logical type contains NUL bytes: '" +
                printable_string(type_name) +
                "'. Node JSON: " + row.dump()
            );
        }

        return Node{
            .global_id = boost::lexical_cast<boost::uuids::uuid>(uuid_str),
            .type = db_context_.schema->node_type_id(type_name),
            .labels = labels,
            .properties = get_properties(row)
        };
    }

    [[nodiscard]] Edge json_to_edge(const json& row) const {
        CPGDB_PROFILE_FUNCTION();

        const std::string uuid_str = get_uuid_string(row, "Edge");

        const std::string edge_type_name =
            get_required_string_property(row, "type", "Neo4j edge");

        const std::string start_uuid =
            get_required_string_property(row, "start", "Neo4j edge");

        const std::string end_uuid =
            get_required_string_property(row, "end", "Neo4j edge");

        return Edge{
            .global_id = boost::lexical_cast<boost::uuids::uuid>(uuid_str),
            .type = db_context_.schema->edge_type_id(edge_type_name),
            .src = boost::lexical_cast<boost::uuids::uuid>(start_uuid),
            .dst = boost::lexical_cast<boost::uuids::uuid>(end_uuid),
            .properties = get_properties(row)
        };
    }
};

#endif // CPGDB_LOCALLYDEPLOYEDNEO4JRESPONSEPROCESSOR_HPP
