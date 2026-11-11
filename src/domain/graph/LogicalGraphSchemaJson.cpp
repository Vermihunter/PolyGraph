#include "domain/graph/LogicalGraphSchemaJson.hpp"
#include <set>
namespace polygraph::domain {
    graph_t logical_schema_from_json(const json &j) {
        const std::unordered_map<std::string, PropertyType> types{
                {"string", PropertyType::string},   {"integer", PropertyType::integer},
                {"boolean", PropertyType::boolean}, {"float", PropertyType::_float},
                {"date", PropertyType::date},       {"datetime", PropertyType::datetime}};
        const auto properties = [&](const json &items) {
            std::vector<Property> result;
            std::set<std::string> names;
            for (const auto &p: items) {
                const auto name = p.at("name").get<std::string>();
                if (!names.insert(name).second)
                    throw std::invalid_argument("Duplicate logical property " + name);
                result.push_back({name, types.at(p.at("type").get<std::string>())});
            }
            return result;
        };
        auto result = std::make_unique<LogicalGraphSchema>();
        result->add_node_type("Proxy");
        std::set<std::string> nodes{"Proxy"}, edges;
        for (const auto &n: j.at("nodeTypes")) {
            const auto name = n.at("name").get<std::string>();
            if (!nodes.insert(name).second)
                throw std::invalid_argument("Duplicate logical node type " + name);
            result->add_node_type(name, properties(n.value("properties", json::array())));
        }
        for (const auto &e: j.at("relations")) {
            const auto name = e.at("name").get<std::string>();
            if (!edges.insert(name).second)
                throw std::invalid_argument("Duplicate logical edge type " + name);
            result->add_edge_type(name, result->node_type_id(e.at("from")), result->node_type_id(e.at("to")),
                                  e.value("directed", true), properties(e.value("properties", json::array())));
        }
        return result;
    }
} // namespace polygraph::domain
