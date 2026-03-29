//
// Created by akos on 6. 3. 2026.
//

#ifndef CPGDB_RECORDJSONSERIALIZER_HPP
#define CPGDB_RECORDJSONSERIALIZER_HPP

#include <variant>

#include <data_model/Record.hpp>
#include <data_model/Value.hpp>
#include <utilities/json.hpp>

//using json = nlohmann::json;
// Forward declarations for the compiler
// --- unique_ptr Handling (Must be in nlohmann namespace) ---
namespace nlohmann {
    template <typename T>
    struct adl_serializer<std::unique_ptr<T>> {
        static void to_json(json& j, const std::unique_ptr<T>& ptr) {
            if (ptr) j = *ptr; else j = nullptr;
        }
        static void from_json(const json& j, std::unique_ptr<T>& ptr) {
            if (j.is_null()) ptr = nullptr;
            else ptr = std::make_unique<T>(j.get<T>());
        }
    };
}

// --- Implementation ---

inline void to_json(json& j, const Node& n) {
    j = json::object(); // Explicitly create object to avoid ambiguity
    j["id"] = n.id;
    j["labels"] = n.labels;
    j["properties"] = n.properties;
}

inline void from_json(const json& j, Node& n) {
    n.id = j.at("id").get<decltype(n.id)>();
    n.labels = j.at("labels").get<std::vector<std::string>>();
    n.properties = j.at("properties").get<std::unordered_map<std::string, std::unique_ptr<Value>>>();
}

inline void to_json(json& j, const Edge& e) {
    j = json::object();
    j["id"] = e.id;
    j["type"] = e.type;
    j["src"] = e.src;
    j["dst"] = e.dst;
    j["properties"] = e.properties;
}

inline void from_json(const json& j, Edge& e) {
    e.id = j.at("id").get<decltype(e.id)>();
    e.type = j.at("type").get<std::string>();
    e.src = j.at("src").get<decltype(e.src)>();
    e.dst = j.at("dst").get<decltype(e.dst)>();
    e.properties = j.at("properties").get<std::unordered_map<std::string, std::unique_ptr<Value>>>();
}

inline void to_json(json& j, const Path& p) {
    j = json::object();
    j["nodes"] = p.nodes;
    j["edges"] = p.edges;
}

inline void from_json(const json& j, Path& p) {
    p.nodes = j.at("nodes").get<std::vector<std::unique_ptr<Node>>>();
    p.edges = j.at("edges").get<std::vector<std::unique_ptr<Edge>>>();
}

inline void to_json(json& j, const Value& v) {
    std::visit([&j](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            j = nullptr;
        } else {
            j = arg;
        }
    }, v.data);
}

inline void from_json(const json& j, Value& v) {
    if (j.is_null()) {
        v.data = std::monostate{};
    } else if (j.is_boolean()) {
        v.data = j.get<bool>();
    } else if (j.is_number_integer()) {
        v.data = j.get<int64_t>();
    } else if (j.is_number_float()) {
        v.data = j.get<double>();
    } else if (j.is_string()) {
        v.data = j.get<std::string>();
    } else if (j.is_array()) {
        // Checking for binary (std::vector<uint8_t>) vs List
        if (j.is_binary()) {
            v.data = j.get_binary();
        } else {
            v.data = j.get<List>();
        }
    } else if (j.is_object()) {
        if (j.contains("nodes") && j.contains("edges")) {
            v.data = j.get<Path>();
        } else if (j.contains("labels") && j.contains("id")) {
            v.data = j.get<Node>();
        } else if (j.contains("src") && j.contains("dst")) {
            v.data = j.get<Edge>();
        } else {
            v.data = j.get<std::unordered_map<std::string, std::unique_ptr<Value>>>();
        }
    }
}

inline void to_json(json& j, const Record& r) {
    j = json::object();
    j["fields"] = r.fields;
}

inline void from_json(const json& j, Record& r) {
    r.fields = j.at("fields").get<std::vector<Value>>();
}


#endif //CPGDB_RECORDJSONSERIALIZER_HPP