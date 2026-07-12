/**
 * @file src/serialization/RecordJsonSerializer.cpp
 * @brief Implementation for record json serializer in the serialization module.
 */

#include "serialization/RecordJsonSerializer.hpp"

#include <boost/uuid/string_generator.hpp>
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cpgdb {
    namespace {

        [[nodiscard]] GlobalId json_to_uuid(const json &value) {
            return boost::uuids::string_generator{}(value.get<std::string>());
        }

    } // namespace

    void to_json(json &value, const Node &node) {
        value = json{
                {"id", node.global_id}, {"type", node.type}, {"labels", node.labels}, {"properties", node.properties}};
    }

    void from_json(const json &value, Node &node) {
        node.global_id = json_to_uuid(value.at("id"));
        node.type = value.value("type", TypeId{});
        node.labels = value.at("labels").get<std::vector<std::string>>();
        node.properties = value.at("properties").get<std::unordered_map<std::string, std::unique_ptr<Value>>>();
    }

    void to_json(json &value, const Edge &edge) {
        value = json{{"global_id", edge.global_id},
                     {"type", edge.type},
                     {"src", edge.src},
                     {"dst", edge.dst},
                     {"properties", edge.properties}};
    }

    void from_json(const json &value, Edge &edge) {
        edge.global_id = json_to_uuid(value.at("global_id"));
        edge.type = value.at("type").get<TypeId>();
        edge.src = json_to_uuid(value.at("src"));
        edge.dst = json_to_uuid(value.at("dst"));
        edge.properties = value.at("properties").get<Map>();
    }

    void to_json(json &value, const Path &path) { value = json{{"nodes", path.nodes}, {"edges", path.edges}}; }

    void from_json(const json &value, Path &path) {
        path.nodes = value.at("nodes").get<std::vector<std::unique_ptr<Node>>>();
        path.edges = value.at("edges").get<std::vector<std::unique_ptr<Edge>>>();
    }

    void to_json(json &value, const Value &source) {
        std::visit(
                [&value](const auto &item) {
                    using Item = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<Item, std::monostate>) {
                        value = nullptr;
                    } else {
                        value = item;
                    }
                },
                source.data);
    }

    void from_json(const json &value, Value &target) {
        if (value.is_null()) {
            target.data = std::monostate{};
        } else if (value.is_boolean()) {
            target.data = value.get<bool>();
        } else if (value.is_number_integer()) {
            target.data = value.get<std::int64_t>();
        } else if (value.is_number_float()) {
            target.data = value.get<double>();
        } else if (value.is_string()) {
            target.data = value.get<std::string>();
        } else if (value.is_binary()) {
            target.data = value.get_binary();
        } else if (value.is_array()) {
            target.data = value.get<List>();
        } else if (value.contains("nodes") && value.contains("edges")) {
            target.data = value.get<Path>();
        } else if (value.contains("labels") && value.contains("id")) {
            target.data = value.get<Node>();
        } else if (value.contains("src") && value.contains("dst")) {
            target.data = value.get<Edge>();
        } else {
            target.data = value.get<Map>();
        }
    }

    void to_json(json &value, const Record &record) { value = json{{"fields", record.fields}}; }

    void from_json(const json &value, Record &record) { record.fields = value.at("fields").get<std::vector<Value>>(); }

} // namespace cpgdb
