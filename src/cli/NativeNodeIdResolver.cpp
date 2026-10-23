/**
 * @file src/cli/NativeNodeIdResolver.cpp
 * @brief Sparse, lazy lookup for polygraph.native-id-map node records.
 */

#include "cli/NativeNodeIdResolver.hpp"

#include "utilities/json.hpp"
#include "utilities/uuid.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace polygraph::cli {
    namespace {

        [[nodiscard]] GlobalId json_global_id(const json &value) {
            if (value.is_number_unsigned()) {
                return value.get<GlobalId>();
            }
            if (value.is_number_integer()) {
                return global_id_from_signed_storage(value.get<std::int64_t>());
            }
            if (value.is_string()) {
                return global_id_from_string(value.get<std::string>());
            }
            throw std::runtime_error("Global ID must be an integer or decimal string");
        }

    } // namespace

    NativeNodeIdResolver::NativeNodeIdResolver(std::filesystem::path path, const std::size_t index_stride) :
        path_(std::move(path)) {
        if (index_stride == 0) {
            throw std::invalid_argument("Native ID map index stride must be greater than zero");
        }
        build_sparse_index(index_stride);
    }

    GlobalId NativeNodeIdResolver::resolve(const GlobalId input_id) const {
        std::scoped_lock lock(mutex_);
        if (const auto cached = cache_.find(input_id); cached != cache_.end()) {
            return cached->second;
        }

        GlobalId resolved = input_id;
        if (!sparse_index_.empty()) {
            const auto upper = std::ranges::upper_bound(sparse_index_, input_id, {}, &IndexEntry::old_id);
            std::streamoff offset = 0;
            if (upper != sparse_index_.begin()) {
                offset = std::prev(upper)->offset;
            }

            std::ifstream input(path_);
            if (!input) {
                throw std::runtime_error("Could not reopen native ID map: " + path_.string());
            }
            input.seekg(offset);
            if (!input) {
                throw std::runtime_error("Could not seek native ID map: " + path_.string());
            }

            std::string line;
            while (std::getline(input, line)) {
                if (line.empty()) {
                    continue;
                }
                const json record = json::parse(line);
                const auto kind = record.value("record", std::string{});
                if (kind == "relationship") {
                    break;
                }
                if (kind != "node") {
                    continue;
                }

                const GlobalId old_id = json_global_id(record.at("old"));
                if (old_id == input_id) {
                    resolved = json_global_id(record.at("new"));
                    break;
                }
                if (old_id > input_id) {
                    break;
                }
            }
        }

        cache_.emplace(input_id, resolved);
        return resolved;
    }

    std::size_t NativeNodeIdResolver::cached_entries() const {
        std::scoped_lock lock(mutex_);
        return cache_.size();
    }

    void NativeNodeIdResolver::build_sparse_index(const std::size_t index_stride) {
        std::ifstream input(path_);
        if (!input) {
            throw std::runtime_error("Could not open native ID map: " + path_.string());
        }

        std::optional<GlobalId> previous_old_id;
        bool metadata_seen = false;
        std::size_t node_count = 0;
        std::string line;
        while (input) {
            const auto position = input.tellg();
            if (!std::getline(input, line)) {
                break;
            }
            if (line.empty()) {
                continue;
            }

            const json record = json::parse(line);
            const auto kind = record.value("record", std::string{});
            if (kind == "metadata") {
                if (metadata_seen || record.value("format", std::string{}) != "polygraph.native-id-map" ||
                    record.value("version", 0) != 1) {
                    throw std::runtime_error("Unsupported native ID map: " + path_.string());
                }
                metadata_seen = true;
                continue;
            }
            if (kind == "relationship") {
                break;
            }
            if (kind != "node") {
                continue;
            }
            if (!metadata_seen) {
                throw std::runtime_error("Native ID map metadata must precede node records: " + path_.string());
            }

            const GlobalId old_id = json_global_id(record.at("old"));
            if (previous_old_id && old_id <= *previous_old_id) {
                throw std::runtime_error("Native ID map node records must be strictly ordered by old ID: " +
                                         path_.string());
            }
            previous_old_id = old_id;

            if (node_count % index_stride == 0) {
                sparse_index_.push_back(IndexEntry{.old_id = old_id, .offset = position - std::streampos{0}});
            }
            ++node_count;
        }

        if (!metadata_seen) {
            throw std::runtime_error("Native ID map metadata record is missing: " + path_.string());
        }
        sparse_index_.shrink_to_fit();
    }

} // namespace polygraph::cli
