/**
 * @file src/data_access/adapters/Neo4J/LocallyDeployedNeo4JResponseProcessor.cpp
 * @brief Implementation for locally deployed neo4 jresponse processor in the graph-store access
 * module.
 */

#include "data_access/adapters/Neo4J/response_processors/LocallyDeployedNeo4JResponseProcessor.hpp"

#include "data_access/adapters/Neo4J/response_processors/Neo4JResponseProcessor.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "serialization/RecordJsonSerializer.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/Profiling.hpp"
#include "utilities/uuid.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cpgdb {

    struct LocallyDeployedNeo4JResponseProcessor::Impl final : Neo4JResponseProcessor {
        DatabaseContext &db_context_;

    public:
        explicit Impl(DatabaseContext &db_context) : db_context_(db_context) {}

        [[nodiscard]] query_result_t process(const json &response, ResultParseType parse_type) const override {
            CPGDB_PROFILE_FUNCTION();

            if (response.contains("errors") && !response["errors"].empty()) {
                throw std::runtime_error("Neo4j returned errors: " + response["errors"].dump());
            }

            if (!response.contains("results") || response["results"].empty()) {
                throw std::runtime_error("Neo4j response does not contain results");
            }

            const auto &result_block = response["results"][0];

            if (!result_block.contains("data")) {
                throw std::runtime_error("Neo4j result block does not contain data");
            }

            return construct_result(result_block, parse_type);
        }

    private:
        [[nodiscard]] query_result_t construct_result(const json &result_block, ResultParseType parse_type) const {
            CPGDB_PROFILE_FUNCTION();

            auto result = std::make_unique<QueryResult>();

            const auto &data_rows = result_block.at("data");

            for (const auto &data_entry: data_rows) {
                if (!data_entry.contains("row")) {
                    throw std::runtime_error("Neo4j data entry does not contain row");
                }

                const auto &row = data_entry.at("row");

                auto record = std::make_unique<Record>();

                switch (parse_type) {
                    case ResultParseType::Node: {
                        require_row_size_at_least(row, 1, "NODE");
                        record->fields.emplace_back(json_to_node(row[0]));
                        break;
                    }

                    case ResultParseType::Relationship: {
                        require_row_size_at_least(row, 1, "RELATIONSHIP");
                        record->fields.emplace_back(json_to_edge(row[0]));
                        break;
                    }

                    case ResultParseType::AdjacencyRow: {
                        require_row_size_at_least(row, 3, "ADJACENCY_ROW");
                        record->fields.emplace_back(json_to_node(row[0]));
                        record->fields.emplace_back(json_to_node(row[1]));
                        record->fields.emplace_back(json_to_edge(row[2]));
                        break;
                    }

                    default:
                        throw std::invalid_argument("Unknown ResultParseType");
                }

                result->data.push_back(std::move(record));
            }

            return result;
        }

        static void require_row_size_at_least(const json &row, std::size_t expected, const std::string &parse_name) {
            if (!row.is_array()) {
                throw std::runtime_error(parse_name + " expected row to be an array");
            }

            if (row.size() < expected) {
                throw std::runtime_error(parse_name + " expected at least " + std::to_string(expected) +
                                         " columns, got " + std::to_string(row.size()));
            }
        }

        static std::optional<GlobalId> get_global_id_property_opt(const json &row, const std::string &name) {
            const json *value = nullptr;
            if (row.contains(name)) {
                value = &row[name];
            } else if (row.contains("properties") && row["properties"].is_object() &&
                       row["properties"].contains(name)) {
                value = &row["properties"][name];
            }
            if (value == nullptr || value->is_null()) {
                return std::nullopt;
            }
            if (value->is_number_unsigned()) {
                return value->get<GlobalId>();
            }
            if (value->is_number_integer()) {
                return global_id_from_signed_storage(value->get<std::int64_t>());
            }
            if (value->is_string()) {
                return global_id_from_string(value->get<std::string>());
            }
            throw std::runtime_error("Neo4j ID property '" + name + "' has unsupported JSON type: " + value->dump());
        }

        [[nodiscard]] GlobalId native_global_id(const json &native_value) const {
            if (!native_value.is_number_integer() && !native_value.is_number_unsigned()) {
                throw std::runtime_error("Neo4j native ID is not an integer: " + native_value.dump());
            }
            const auto native_id = native_value.get<std::uint64_t>();
            return insertion::BackendEncodedId::from_local_id(native_id, NEO4J, db_context_.id_routing_backends);
        }

        [[nodiscard]] GlobalId logical_node_id(const json &row, const bool is_proxy) const {
            if (is_proxy) {
                const auto stored = get_global_id_property_opt(row, "uuid");
                if (!stored.has_value()) {
                    throw std::runtime_error("Neo4j proxy node does not contain numeric uuid: " + row.dump());
                }
                return *stored;
            }
            if (!row.contains("native_id")) {
                throw std::runtime_error("Neo4j real node does not contain native_id: " + row.dump());
            }
            return native_global_id(row["native_id"]);
        }


        static bool get_bool_property(const json &row, const std::string &name, bool default_value = false) {
            if (row.contains(name) && row[name].is_boolean()) {
                return row[name].get<bool>();
            }

            if (row.contains("properties") && row["properties"].is_object() && row["properties"].contains(name) &&
                row["properties"][name].is_boolean()) {
                return row["properties"][name].get<bool>();
            }

            return default_value;
        }

        static std::optional<std::string> get_string_property_opt(const json &row, const std::string &name) {
            if (row.contains(name) && row[name].is_string()) {
                auto result = row[name].get<std::string>();

                if (!result.empty()) {
                    return result;
                }
            }

            if (row.contains("properties") && row["properties"].is_object() && row["properties"].contains(name) &&
                row["properties"][name].is_string()) {
                auto result = row["properties"][name].get<std::string>();

                if (!result.empty()) {
                    return result;
                }
            }

            return std::nullopt;
        }

        static std::string get_required_string_property(const json &row, const std::string &name,
                                                        const std::string &entity_name) {
            auto value = get_string_property_opt(row, name);

            if (!value.has_value()) {
                throw std::runtime_error(entity_name + " JSON does not contain string property: " + name +
                                         ". JSON: " + row.dump());
            }

            return *value;
        }

        static std::vector<std::string> get_labels(const json &row) {
            if (row.contains("labels") && row["labels"].is_array()) {
                return row["labels"].get<std::vector<std::string>>();
            }

            return {};
        }

        static std::optional<std::string> first_logical_label(const std::vector<std::string> &labels) {
            for (const auto &label: labels) {
                if (label != "Proxy" && label != "CPGDBVertex" && label != "__GraphNode") {
                    return label;
                }
            }

            return std::nullopt;
        }

        static bool has_label(const std::vector<std::string> &labels, const std::string &wanted) {
            return std::find(labels.begin(), labels.end(), wanted) != labels.end();
        }

        static bool contains_nul_byte(const std::string &value) { return value.find('\0') != std::string::npos; }

        static std::string printable_string(const std::string &value) {
            std::string out;
            out.reserve(value.size());

            for (unsigned char c: value) {
                if (c == '\0') {
                    out += "\\0";
                } else {
                    out.push_back(static_cast<char>(c));
                }
            }

            return out;
        }

        static Map get_properties(const json &row) {
            if (row.contains("properties") && row["properties"].is_object()) {
                return row["properties"].get<Map>();
            }

            return row.get<Map>();
        }

        [[nodiscard]] Node json_to_node(const json &row) const {
            CPGDB_PROFILE_FUNCTION();

            const std::vector<std::string> labels = get_labels(row);
            const auto explicit_type = get_string_property_opt(row, "type");
            const auto remote_type = get_string_property_opt(row, "remote-type");
            const bool is_proxy = get_bool_property(row, "is_proxy", false) || has_label(labels, "Proxy") ||
                                  explicit_type.value_or("") == "Proxy";

            std::string type_name;
            if (is_proxy) {
                if (!remote_type.has_value()) {
                    throw std::runtime_error("Neo4j proxy node does not contain remote-type: " + row.dump());
                }
                type_name = *remote_type;
            } else {
                auto label_type = first_logical_label(labels);
                if (label_type.has_value()) {
                    type_name = *label_type;
                } else if (explicit_type.has_value() && *explicit_type != "Proxy") {
                    type_name = *explicit_type;
                } else {
                    throw std::runtime_error("Neo4j node does not contain usable logical type: " + row.dump());
                }
            }

            if (contains_nul_byte(type_name)) {
                throw std::runtime_error("Neo4j node logical type contains NUL bytes: '" + printable_string(type_name) +
                                         "'. Node JSON: " + row.dump());
            }

            return Node{.global_id = logical_node_id(row, is_proxy),
                        .type = db_context_.schema->node_type_id(type_name),
                        .labels = labels,
                        .properties = get_properties(row)};
        }

        [[nodiscard]] GlobalId logical_endpoint_id(const json &row, const std::string &prefix) const {
            const bool is_proxy = row.value(prefix + "_is_proxy", false);
            if (is_proxy) {
                const auto stored = get_global_id_property_opt(row, prefix + "_uuid");
                if (!stored.has_value()) {
                    throw std::runtime_error("Neo4j edge proxy endpoint is missing " + prefix + "_uuid: " + row.dump());
                }
                return *stored;
            }
            const auto native = row.find(prefix + "_native_id");
            if (native == row.end()) {
                throw std::runtime_error("Neo4j edge is missing " + prefix + "_native_id: " + row.dump());
            }
            return native_global_id(*native);
        }

        [[nodiscard]] Edge json_to_edge(const json &row) const {
            CPGDB_PROFILE_FUNCTION();

            const auto stored_id = get_global_id_property_opt(row, "uuid");
            const GlobalId edge_id = stored_id.has_value() ? *stored_id : native_global_id(row.at("native_id"));
            const std::string edge_type_name = get_required_string_property(row, "type", "Neo4j edge");

            return Edge{.global_id = edge_id,
                        .type = db_context_.schema->edge_type_id(edge_type_name),
                        .src = logical_endpoint_id(row, "start"),
                        .dst = logical_endpoint_id(row, "end"),
                        .properties = get_properties(row)};
        }
    };

    LocallyDeployedNeo4JResponseProcessor::LocallyDeployedNeo4JResponseProcessor(DatabaseContext &database_context) :
        impl_(std::make_unique<Impl>(database_context)) {}

    LocallyDeployedNeo4JResponseProcessor::~LocallyDeployedNeo4JResponseProcessor() = default;

    query_result_t LocallyDeployedNeo4JResponseProcessor::process(const json &response,
                                                                  ResultParseType parse_type) const {
        return impl_->process(response, parse_type);
    }

} // namespace cpgdb
