/**
 * @file src/cli/GraphDataReader.cpp
 * @brief Graph data input implementation.
 */

#include "cli/GraphDataReader.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpgdb::cli {
    namespace {

        [[nodiscard]] const json &required_array(const json &object, std::string_view key) {
            const auto iterator = object.find(std::string(key));
            if (iterator == object.end() || !iterator->is_array()) {
                throw std::runtime_error("Expected JSON array field: " + std::string(key));
            }
            return *iterator;
        }

        [[nodiscard]] bool is_json_lines(const std::string &path) {
            const auto extension = std::filesystem::path(path).extension().string();
            return extension == ".jsonl" || extension == ".ndjson";
        }

        [[nodiscard]] std::string record_kind(const json &record) {
            if (const auto kind = record.find("kind"); kind != record.end() && kind->is_string()) {
                std::string value = kind->get<std::string>();
                std::ranges::transform(value, value.begin(), [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
                return value;
            }
            if (record.contains("from") && record.contains("to")) {
                return "relationship";
            }
            if (record.contains("uuid") && record.contains("type")) {
                return "node";
            }
            return "metadata";
        }


        [[nodiscard]] json read_json_lines_metadata(const std::string &path) {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("Could not open graph data file: " + path);
            }

            json metadata = json::object();
            std::string line;
            std::size_t line_number = 0;
            while (std::getline(input, line)) {
                ++line_number;
                const auto first = std::ranges::find_if(
                        line, [](const unsigned char character) { return !std::isspace(character); });
                if (first == line.end() || *first == '#') {
                    continue;
                }
                json record;
                try {
                    record = json::parse(line);
                } catch (const json::exception &error) {
                    throw std::runtime_error("Invalid graph JSONL at " + path + ':' + std::to_string(line_number) +
                                             ": " + error.what());
                }
                if (record_kind(record) != "metadata") {
                    continue;
                }
                record.erase("kind");
                metadata.update(record);
            }
            return metadata;
        }

        [[nodiscard]] json read_monolithic_metadata(const std::string &path) {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("Could not open graph data file: " + path);
            }
            json metadata = json::parse(input);
            metadata.erase("nodes");
            metadata.erase("relationships");
            return metadata;
        }

        void read_json_lines(const std::string &path, const GraphDataReader::RecordVisitor &on_node,
                             const GraphDataReader::RecordVisitor &on_relationship) {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("Could not open graph data file: " + path);
            }

            std::string line;
            std::size_t line_number = 0;
            bool relationship_seen = false;
            while (std::getline(input, line)) {
                ++line_number;
                const auto first = std::ranges::find_if(
                        line, [](const unsigned char character) { return !std::isspace(character); });
                if (first == line.end() || *first == '#') {
                    continue;
                }

                json record;
                try {
                    record = json::parse(line);
                } catch (const json::exception &error) {
                    throw std::runtime_error("Invalid graph JSONL at " + path + ':' + std::to_string(line_number) +
                                             ": " + error.what());
                }

                const std::string kind = record_kind(record);
                record.erase("kind");
                if (kind == "node") {
                    if (relationship_seen) {
                        throw std::runtime_error("Graph JSONL must place all nodes before relationships: " + path +
                                                 ':' + std::to_string(line_number));
                    }
                    on_node(record);
                } else if (kind == "relationship" || kind == "edge") {
                    relationship_seen = true;
                    on_relationship(record);
                }
            }
        }

        void read_monolithic_json(const std::string &path, const GraphDataReader::RecordVisitor &on_node,
                                  const GraphDataReader::RecordVisitor &on_relationship) {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("Could not open graph data file: " + path);
            }
            const json data = json::parse(input);
            for (const auto &node: required_array(data, "nodes")) {
                on_node(node);
            }
            for (const auto &relationship: required_array(data, "relationships")) {
                on_relationship(relationship);
            }
        }

    } // namespace


    json GraphDataReader::metadata(const std::string &path) {
        return is_json_lines(path) ? read_json_lines_metadata(path) : read_monolithic_metadata(path);
    }

    void GraphDataReader::for_each(const std::string &path, const RecordVisitor &on_node,
                                   const RecordVisitor &on_relationship) {
        if (is_json_lines(path)) {
            read_json_lines(path, on_node, on_relationship);
        } else {
            read_monolithic_json(path, on_node, on_relationship);
        }
    }

} // namespace cpgdb::cli
