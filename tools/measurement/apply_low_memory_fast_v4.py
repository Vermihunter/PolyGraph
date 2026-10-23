#!/usr/bin/env python3
'''Apply low-memory / cold-Arango BFS optimizations to the current PolyGraph tree.

The bootstrap Git patch that installs this script contains only NEW files, so
`git apply --check` is independent of line numbers in hot source files. This
script edits the current source using exact semantic anchors and validates all
transformations before writing anything.
'''
from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]

HOT_FILES = (
    "include/algorithms/bfs/BfsOptions.hpp",
    "include/algorithms/bfs/BfsResult.hpp",
    "src/algorithms/bfs/BfsExecutor.cpp",
    "src/data_access/adapters/ArangoDB/ArangoQueryVisitor.cpp",
    "src/server/controllers/TraversalController.cpp",
    "src/server/services/TraversalService.cpp",
)


def read(rel: str) -> str:
    path = ROOT / rel
    if not path.is_file():
        raise RuntimeError(f"required source file does not exist: {rel}")
    return path.read_text(encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str, already: str | None = None) -> str:
    if already is not None and already in text:
        return text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one source anchor, found {count}")
    return text.replace(old, new, 1)


def add_after_once(text: str, anchor: str, addition: str, label: str, already: str) -> str:
    if already in text:
        return text
    count = text.count(anchor)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one source anchor, found {count}")
    return text.replace(anchor, anchor + addition, 1)


def transform_bfs_options(text: str) -> str:
    return add_after_once(
        text,
        "    bool return_predecessors = true;\n",
        "    /// When false, summary BFS keeps only compact visited membership, not a distance hash map.\n"
        "    bool materialize_distances = true;\n",
        "BfsOptions materialize_distances",
        "bool materialize_distances = true;",
    )


def transform_bfs_result(text: str) -> str:
    if "#include <cstddef>" not in text:
        text = replace_once(
            text,
            "#include <cstdint>\n",
            "#include <cstddef>\n#include <cstdint>\n",
            "BfsResult cstddef include",
        )

    if "reachable_vertex_count" in text:
        return text

    lines = text.splitlines(keepends=True)
    matches = [
        i for i, line in enumerate(lines)
        if "std::unordered_map<GlobalId, GlobalId" in line and "predecessor;" in line
    ]
    if len(matches) != 1:
        raise RuntimeError(
            f"BfsResult predecessor anchor: expected exactly one matching line, found {len(matches)}"
        )

    i = matches[0] + 1
    block = (
        "    /// Reachable vertex count when the distance map is intentionally not materialized.\n"
        "    std::size_t reachable_vertex_count = 0;\n\n"
        "    [[nodiscard]] std::size_t vertex_count() const noexcept {\n"
        "        return reachable_vertex_count == 0 ? distance.size() : reachable_vertex_count;\n"
        "    }\n"
    )
    lines[i:i] = [block]
    return "".join(lines)


FLAT_ID_SET = r'''/**
 * @file include/utilities/FlatIdSet.hpp
 * @brief Compact open-addressing set for exact GlobalId membership tests.
 */

#ifndef POLYGRAPH_UTILITIES_FLAT_ID_SET_HPP
#define POLYGRAPH_UTILITIES_FLAT_ID_SET_HPP

#include "data_model/EntityId.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace polygraph {

class FlatIdSet final {
  public:
    FlatIdSet() = default;
    explicit FlatIdSet(const std::size_t expected_size) { reserve(expected_size); }

    void reserve(const std::size_t expected_size) {
        if (expected_size == 0) {
            return;
        }
        const auto required =
                static_cast<std::size_t>(static_cast<double>(expected_size) / max_load_factor) + 1;
        if (required > buckets_.size()) {
            rehash(std::bit_ceil(std::max(minimum_capacity, required)));
        }
    }

    [[nodiscard]] bool insert(const GlobalId id) {
        if (buckets_.empty()) {
            rehash(minimum_capacity);
        }
        if (static_cast<double>(size_ + 1) >
            static_cast<double>(buckets_.size()) * max_load_factor) {
            rehash(buckets_.size() * 2);
        }

        const std::size_t mask = buckets_.size() - 1;
        std::size_t slot = hash(id) & mask;
        while (occupied_[slot] != 0U) {
            if (buckets_[slot] == id) {
                return false;
            }
            slot = (slot + 1) & mask;
        }

        buckets_[slot] = id;
        occupied_[slot] = 1U;
        ++size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

  private:
    static constexpr std::size_t minimum_capacity = 16;
    static constexpr double max_load_factor = 0.70;

    [[nodiscard]] static std::size_t hash(const GlobalId id) noexcept {
        std::uint64_t value = static_cast<std::uint64_t>(id);
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<std::size_t>(value);
    }

    void rehash(const std::size_t requested_capacity) {
        const std::size_t capacity =
                std::bit_ceil(std::max(minimum_capacity, requested_capacity));

        std::vector<GlobalId> old_buckets = std::move(buckets_);
        std::vector<std::uint8_t> old_occupied = std::move(occupied_);

        buckets_.resize(capacity);
        occupied_.assign(capacity, 0U);
        size_ = 0;

        for (std::size_t i = 0; i < old_buckets.size(); ++i) {
            if (old_occupied[i] != 0U) {
                (void) insert(old_buckets[i]);
            }
        }
    }

    std::vector<GlobalId> buckets_;
    std::vector<std::uint8_t> occupied_;
    std::size_t size_{};
};

} // namespace polygraph

#endif // POLYGRAPH_UTILITIES_FLAT_ID_SET_HPP
'''


def transform_bfs_executor(text: str) -> str:
    if '#include "utilities/FlatIdSet.hpp"' not in text:
        text = replace_once(
            text,
            '#include "infrastructure/scheduling/BackendRequestScheduler.hpp"\n'
            '#include "utilities/Profiling.hpp"\n',
            '#include "infrastructure/scheduling/BackendRequestScheduler.hpp"\n'
            '#include "utilities/FlatIdSet.hpp"\n'
            '#include "utilities/Profiling.hpp"\n',
            "BfsExecutor FlatIdSet include",
        )

    text = replace_once(
        text,
        "        BfsResult result;\n"
        "        result.distance.reserve(expected_vertex_count);\n"
        "        result.distance.emplace(source.global_id, 0U);\n",
        "        BfsResult result;\n"
        "        FlatIdSet compact_visited;\n"
        "        if (options.materialize_distances) {\n"
        "            result.distance.reserve(expected_vertex_count);\n"
        "            result.distance.emplace(source.global_id, 0U);\n"
        "        } else {\n"
        "            compact_visited.reserve(expected_vertex_count);\n"
        "            (void) compact_visited.insert(source.global_id);\n"
        "            result.reachable_vertex_count = 1;\n"
        "        }\n\n"
        "        const auto visited_size = [&]() -> std::size_t {\n"
        "            return options.materialize_distances ? result.distance.size() : compact_visited.size();\n"
        "        };\n",
        "BfsExecutor compact visited initialization",
        "FlatIdSet compact_visited;",
    )

    text = replace_once(
        text,
        "                                const auto [iterator, inserted] =\n"
        "                                        result.distance.emplace(neighbor.global_id, depth + 1);\n"
        "                                (void) iterator;\n"
        "                                if (!inserted) {\n",
        "                                bool inserted = false;\n"
        "                                if (options.materialize_distances) {\n"
        "                                    const auto [iterator, was_inserted] =\n"
        "                                            result.distance.emplace(neighbor.global_id, depth + 1);\n"
        "                                    (void) iterator;\n"
        "                                    inserted = was_inserted;\n"
        "                                } else {\n"
        "                                    inserted = compact_visited.insert(neighbor.global_id);\n"
        "                                }\n"
        "                                if (!inserted) {\n",
        "BfsExecutor compact visited insertion",
        "inserted = compact_visited.insert(neighbor.global_id);",
    )

    text = replace_once(
        text,
        '                expansion_layer.add_end_field("visited_vertex_count", result.distance.size());\n',
        '                expansion_layer.add_end_field("visited_vertex_count", visited_size());\n',
        "BfsExecutor visited metric",
        'expansion_layer.add_end_field("visited_vertex_count", visited_size());',
    )

    text = replace_once(
        text,
        '        EVENT_SCOPE_INFO_FIELD(full_bfs, "result_vertices", result.distance.size());\n'
        '        return result;\n',
        '        result.reachable_vertex_count = visited_size();\n'
        '        EVENT_SCOPE_INFO_FIELD(full_bfs, "result_vertices", result.vertex_count());\n'
        '        return result;\n',
        "BfsExecutor final count",
        'EVENT_SCOPE_INFO_FIELD(full_bfs, "result_vertices", result.vertex_count());',
    )
    return text


def transform_traversal_service(text: str) -> str:
    return add_after_once(
        text,
        "                               .return_predecessors = request.returnPredecessors,\n",
        '                               .materialize_distances = request.responseMode == "full",\n',
        "TraversalService summary distance policy",
        ".materialize_distances = request.responseMode",
    )


def transform_traversal_controller(text: str) -> str:
    text = replace_once(
        text,
        '            EVENT_SCOPE_INFO_FIELD(bfs_layer, "vertices", result.distance.size());\n',
        '            EVENT_SCOPE_INFO_FIELD(bfs_layer, "vertices", result.vertex_count());\n',
        "TraversalController event vertex count",
        'EVENT_SCOPE_INFO_FIELD(bfs_layer, "vertices", result.vertex_count());',
    )
    text = replace_once(
        text,
        '                                    {"vertexCount", timed.result.distance.size()},\n',
        '                                    {"vertexCount", timed.result.vertex_count()},\n',
        "TraversalController summary vertex count",
        '{"vertexCount", timed.result.vertex_count()},',
    )
    return text


ARANGO_HELPERS = r'''        [[nodiscard]] static bool env_flag(const char *name) {
            const char *value = std::getenv(name);
            if (value == nullptr) {
                return false;
            }
            const std::string normalized(value);
            return normalized == "1" || normalized == "true" || normalized == "TRUE" ||
                   normalized == "on" || normalized == "ON" ||
                   normalized == "yes" || normalized == "YES";
        }

        [[nodiscard]] static bool compact_arango_endpoints_enabled() {
            if (std::getenv("POLYGRAPH_ARANGO_COMPACT_ENDPOINTS") != nullptr) {
                return env_flag("POLYGRAPH_ARANGO_COMPACT_ENDPOINTS");
            }
            return env_flag("POLYGRAPH_ARANGO_LOW_MEMORY");
        }

        [[nodiscard]] static std::size_t env_size(const char *name, const std::size_t fallback) {
            const char *value = std::getenv(name);
            if (value == nullptr || *value == '\0') {
                return fallback;
            }
            try {
                const auto parsed = std::stoull(value);
                if (parsed == 0) {
                    throw std::invalid_argument("zero");
                }
                return static_cast<std::size_t>(parsed);
            } catch (...) {
                throw std::invalid_argument(std::string("Invalid positive integer in ") + name + ": " + value);
            }
        }

        void stream_neighbor_candidates_multi_edge(
                std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth,
                std::size_t batch_size, std::size_t max_concurrent_requests,
                std::size_t request_retries, std::size_t retry_delay_ms,
                std::size_t candidate_batch_size, const NeighborBatchConsumer &consumer,
                const BackendRequestScheduling &scheduling) const {
            EVENT_SCOPE_INFO(total_span, "arango.stream_neighbor_candidates.multi_edge",
                             {"depth", depth}, {"frontier_size", frontier.size()},
                             {"batch_size", batch_size},
                             {"max_concurrent_requests", max_concurrent_requests});

            const auto grouped_frontier = group_frontier_by_collection(frontier);
            const auto edge_type_ids = edge_types_for_query(spec);
            if (grouped_frontier.empty() || edge_type_ids.empty()) {
                return;
            }

            json edge_bindings = json::object();
            std::string edge_collections;
            for (std::size_t i = 0; i < edge_type_ids.size(); ++i) {
                const std::string bind_name = "edgeCollection" + std::to_string(i);
                edge_bindings["@" + bind_name] =
                        db_context_.schema->edge_type(edge_type_ids[i]).name;
                if (!edge_collections.empty()) {
                    edge_collections.append(", ");
                }
                edge_collections.append("@@").append(bind_name);
            }

            std::string query =
                    "FOR sourceKey IN @frontier "
                    "LET start = DOCUMENT(@@vertexCollection, sourceKey) "
                    "FILTER start != null "
                    "FOR neighbor IN 1..1 ";
            query.append(arango_direction(spec.direction));
            query.append(" start ");
            query.append(edge_collections);
            query.append(
                    " OPTIONS { uniqueVertices: \"none\" } "
                    "LET parsed = PARSE_IDENTIFIER(neighbor._id) "
                    "LET logicalType = parsed.collection == \"Proxy\" ? "
                    "neighbor[\"remote-type\"] : parsed.collection "
                    "FILTER logicalType != null && logicalType != \"\" ");
            if (compact_arango_endpoints_enabled()) {
                query.append("RETURN [parsed.key, logicalType, true]");
            } else {
                query.append("RETURN [neighbor._id, logicalType]");
            }

            struct Descriptor {
                std::string vertex_collection;
                const std::vector<NodeRef> *group{};
                std::size_t offset{};
                std::size_t count{};
            };

            std::vector<Descriptor> descriptors;
            for (const auto &[vertex_collection, group]: grouped_frontier) {
                for (std::size_t offset = 0; offset < group.size(); offset += batch_size) {
                    descriptors.push_back(
                            Descriptor{.vertex_collection = vertex_collection,
                                       .group = &group,
                                       .offset = offset,
                                       .count = std::min(batch_size, group.size() - offset)});
                }
            }
            if (descriptors.empty()) {
                return;
            }

            const std::size_t worker_count =
                    std::min({max_concurrent_requests, descriptors.size(),
                              scheduler_->config().max_in_flight});
            std::atomic_size_t next{0};
            std::atomic_size_t completed{0};
            std::atomic_size_t rows{0};
            std::atomic_size_t request_bytes{0};
            std::atomic_bool stop{false};
            std::vector<std::exception_ptr> errors(worker_count);
            std::vector<std::thread> workers;
            workers.reserve(worker_count);

            const auto worker =
                    [this, &descriptors, &edge_bindings, &query, &next, &completed, &rows,
                     &request_bytes, &stop, &errors, spec, request_retries, retry_delay_ms,
                     candidate_batch_size, consumer, scheduling](const std::size_t worker_index) {
                try {
                    auto http = make_http_client();
                    while (!stop.load(std::memory_order_relaxed)) {
                        const std::size_t index =
                                next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= descriptors.size()) {
                            break;
                        }
                        const auto &descriptor = descriptors[index];

                        EVENT_SCOPE_INFO(request_span, "arango.stream.multi_edge_query",
                                         {"vertex_collection", descriptor.vertex_collection},
                                         {"group_size", descriptor.count},
                                         {"prepared_index", index},
                                         {"worker_index", worker_index});

                        for (std::size_t attempt = 0;; ++attempt) {
                            try {
                                std::size_t current_request_bytes = 0;
                                const std::size_t row_count =
                                        scheduler_->run(
                                                scheduling, "arango.cursor.multi_edge", [&] {
                                                    json frontier_json = json::array();
                                                    auto &array =
                                                            frontier_json.get_ref<json::array_t &>();
                                                    array.reserve(descriptor.count);
                                                    for (std::size_t i = 0;
                                                         i < descriptor.count; ++i) {
                                                        array.emplace_back(global_id_to_string(
                                                                (*descriptor.group)
                                                                        [descriptor.offset + i]
                                                                                .global_id));
                                                    }

                                                    json bind_vars = edge_bindings;
                                                    bind_vars["@vertexCollection"] =
                                                            descriptor.vertex_collection;
                                                    bind_vars["frontier"] =
                                                            std::move(frontier_json);

                                                    const std::string body =
                                                            build_minimal_cursor_request_body(
                                                                    query, bind_vars);
                                                    current_request_bytes = body.size();
                                                    return send_minimal_request_body_to_consumer(
                                                            http, body,
                                                            descriptor.vertex_collection,
                                                            descriptor.count, spec,
                                                            candidate_batch_size, consumer);
                                                });

                                request_bytes.fetch_add(
                                        current_request_bytes,
                                        std::memory_order_relaxed);
                                rows.fetch_add(row_count, std::memory_order_relaxed);
                                completed.fetch_add(1, std::memory_order_relaxed);
                                request_span.add_end_field("attempts", attempt + 1);
                                request_span.add_end_field(
                                        "request_bytes", current_request_bytes);
                                request_span.add_end_field(
                                        "response_row_count", row_count);
                                request_span.add_end_field("status", "ok");
                                break;
                            } catch (const std::exception &error) {
                                if (attempt >= request_retries) {
                                    request_span.add_end_field("attempts", attempt + 1);
                                    request_span.add_end_field("status", "failed");
                                    request_span.add_end_field("error", error.what());
                                    throw;
                                }
                                http = make_http_client();
                                const std::size_t delay =
                                        retry_delay_ms *
                                        (std::size_t{1}
                                         << std::min(attempt, std::size_t{10}));
                                const std::size_t jitter =
                                        retry_delay_ms == 0 ? 0 : (worker_index % 8) * 7;
                                std::this_thread::sleep_for(
                                        std::chrono::milliseconds(delay + jitter));
                            }
                        }
                    }
                } catch (...) {
                    stop.store(true, std::memory_order_relaxed);
                    errors[worker_index] = std::current_exception();
                }
            };

            for (std::size_t i = 0; i < worker_count; ++i) {
                workers.emplace_back(worker, i);
            }
            for (auto &thread: workers) {
                thread.join();
            }
            for (const auto &error: errors) {
                if (error) {
                    total_span.add_end_field("status", "failed");
                    std::rethrow_exception(error);
                }
            }

            total_span.add_end_field("request_count", descriptors.size());
            total_span.add_end_field(
                    "completed_request_count",
                    completed.load(std::memory_order_relaxed));
            total_span.add_end_field(
                    "response_row_count", rows.load(std::memory_order_relaxed));
            total_span.add_end_field(
                    "request_bytes", request_bytes.load(std::memory_order_relaxed));
            total_span.add_end_field("worker_count", worker_count);
            total_span.add_end_field("status", "ok");
        }

'''


def transform_arango(text: str) -> str:
    if "#include <cstdlib>" not in text:
        text = replace_once(
            text,
            "#include <cstddef>\n",
            "#include <cstddef>\n#include <cstdlib>\n",
            "Arango cstdlib include",
        )

    if "stream_neighbor_candidates_multi_edge(" not in text:
        anchor = (
            "        [[nodiscard]] static std::string "
            "compact_edge_index_query(const bool outbound) {\n"
        )
        count = text.count(anchor)
        if count != 1:
            raise RuntimeError(
                f"Arango helper insertion: expected one compact query anchor, found {count}"
            )
        text = text.replace(anchor, ARANGO_HELPERS + anchor, 1)

    old_stream_prefix = (
        "            if (batch_size == 0 || max_concurrent_requests == 0 || candidate_batch_size == 0) {\n"
        "                throw std::invalid_argument(\"ArangoDB streaming batch sizes and concurrency must be greater than zero\");\n"
        "            }\n\n"
        "            EVENT_SCOPE_INFO(total_span, \"arango.stream_neighbor_candidates\", {\"depth\", depth},\n"
    )
    new_stream_prefix = (
        "            if (batch_size == 0 || max_concurrent_requests == 0 || candidate_batch_size == 0) {\n"
        "                throw std::invalid_argument(\"ArangoDB streaming batch sizes and concurrency must be greater than zero\");\n"
        "            }\n\n"
        "            if (env_flag(\"POLYGRAPH_ARANGO_LOW_MEMORY\")) {\n"
        "                batch_size = std::min(\n"
        "                        batch_size,\n"
        "                        env_size(\"POLYGRAPH_ARANGO_LOW_MEMORY_BATCH_SIZE\", 50'000));\n"
        "                max_concurrent_requests = std::min(\n"
        "                        max_concurrent_requests,\n"
        "                        env_size(\"POLYGRAPH_ARANGO_LOW_MEMORY_MAX_CONCURRENCY\", 4));\n"
        "                single_request_max_frontier =\n"
        "                        std::min(single_request_max_frontier, batch_size);\n"
        "            }\n\n"
        "            if (env_flag(\"POLYGRAPH_ARANGO_MULTI_EDGE_TRAVERSAL\")) {\n"
        "                stream_neighbor_candidates_multi_edge(\n"
        "                        frontier, spec, depth, batch_size, max_concurrent_requests,\n"
        "                        request_retries, retry_delay_ms, candidate_batch_size,\n"
        "                        consumer, scheduling);\n"
        "                return;\n"
        "            }\n\n"
        "            EVENT_SCOPE_INFO(total_span, \"arango.stream_neighbor_candidates\", {\"depth\", depth},\n"
    )
    text = replace_once(
        text,
        old_stream_prefix,
        new_stream_prefix,
        "Arango low-memory stream policy",
        "POLYGRAPH_ARANGO_LOW_MEMORY_BATCH_SIZE",
    )

    old_candidate_query = r'''        [[nodiscard]] static std::string compact_edge_index_candidate_query(const bool outbound) {
            const std::string source_endpoint = outbound ? "edge._from" : "edge._to";
            const std::string neighbor_endpoint = outbound ? "edge._to" : "edge._from";

            // No-predecessor BFS needs only destination IDs. Omitting the source
            // endpoint materially reduces cursor/network volume on high-degree layers.
            return "FOR edge IN @@edgeCollection "
                   "FILTER " +
                   source_endpoint +
                   " IN @frontier "
                   "RETURN [" +
                   neighbor_endpoint + ", @targetType]";
        }
'''
    new_candidate_query = r'''        [[nodiscard]] static std::string compact_edge_index_candidate_query(const bool outbound) {
            const std::string source_endpoint = outbound ? "edge._from" : "edge._to";
            const std::string neighbor_endpoint = outbound ? "edge._to" : "edge._from";

            // No-predecessor BFS needs only destination IDs. Compact mode returns
            // only the document key instead of repeating "Collection/" on every row.
            if (compact_arango_endpoints_enabled()) {
                std::string query = "FOR edge IN @@edgeCollection FILTER ";
                query.append(source_endpoint);
                query.append(" IN @frontier LET neighborId = PARSE_IDENTIFIER(");
                query.append(neighbor_endpoint);
                query.append(") RETURN [neighborId.key, @targetType, true]");
                return query;
            }

            return "FOR edge IN @@edgeCollection "
                   "FILTER " +
                   source_endpoint +
                   " IN @frontier "
                   "RETURN [" +
                   neighbor_endpoint + ", @targetType]";
        }
'''
    text = replace_once(
        text,
        old_candidate_query,
        new_candidate_query,
        "Arango compact candidate query",
        "RETURN [neighborId.key, @targetType, true]",
    )

    old_cursor = r'''        [[nodiscard]] static std::string build_minimal_cursor_request_body(const std::string &query,
                                                                           const json &bind_vars) {
            // Keep the server cursor bounded. The response parser consumes rows while
            // parsing, so the client does not retain a second full JSON result tree.
            return json{{"query", query}, {"bindVars", bind_vars}, {"batchSize", 50000}, {"cache", false}}.dump();
        }
'''
    new_cursor = r'''        [[nodiscard]] static std::string build_minimal_cursor_request_body(const std::string &query,
                                                                           const json &bind_vars) {
            // Keep cursor streaming independently switchable so its effect can be
            // measured without mixing in plan-cache or server-cache changes.
            json request{{"query", query}, {"bindVars", bind_vars}, {"batchSize", 50000}, {"cache", false}};
            if (env_flag("POLYGRAPH_ARANGO_STREAM_CURSOR")) {
                request["options"] = json{{"stream", true}};
            }
            return request.dump();
        }
'''
    text = replace_once(
        text,
        old_cursor,
        new_cursor,
        "Arango cursor streaming switch",
        'POLYGRAPH_ARANGO_STREAM_CURSOR',
    )

    old_detector = r'''        [[nodiscard]] static bool is_candidate_result_row(const json &item) {
            return item.is_array() && item.size() >= 2 && item[0].is_string() && item[1].is_string() &&
                   item[0].get_ref<const std::string &>().find('/') != std::string::npos;
        }
'''
    new_detector = r'''        [[nodiscard]] static bool is_compact_candidate_result_row(const json &item) {
            return item.is_array() && item.size() >= 3 && item[0].is_string() && item[1].is_string() &&
                   item[2].is_boolean() && item[2].get<bool>();
        }

        [[nodiscard]] static bool is_candidate_result_row(const json &item) {
            return is_compact_candidate_result_row(item) ||
                   (item.is_array() && item.size() >= 2 && item[0].is_string() && item[1].is_string() &&
                    item[0].get_ref<const std::string &>().find('/') != std::string::npos);
        }
'''
    text = replace_once(
        text,
        old_detector,
        new_detector,
        "Arango compact candidate detector",
        "is_compact_candidate_result_row",
    )

    old_parse = r'''            if (is_candidate_result_row(item) && !is_minimal_result_row(item)) {
                neighbor_id = global_id_from_document_id(item[0].get<std::string>());
                logical_type_name = item[1].get<std::string>();
            } else {
'''
    new_parse = r'''            if (is_candidate_result_row(item) && !is_minimal_result_row(item)) {
                if (is_compact_candidate_result_row(item)) {
                    neighbor_id = global_id_from_string(item[0].get<std::string>());
                } else {
                    neighbor_id = global_id_from_document_id(item[0].get<std::string>());
                }
                logical_type_name = item[1].get<std::string>();
            } else {
'''
    text = replace_once(
        text,
        old_parse,
        new_parse,
        "Arango compact candidate parse",
        "if (is_compact_candidate_result_row(item))",
    )
    return text


TRANSFORMS = {
    "include/algorithms/bfs/BfsOptions.hpp": transform_bfs_options,
    "include/algorithms/bfs/BfsResult.hpp": transform_bfs_result,
    "src/algorithms/bfs/BfsExecutor.cpp": transform_bfs_executor,
    "src/data_access/adapters/ArangoDB/ArangoQueryVisitor.cpp": transform_arango,
    "src/server/controllers/TraversalController.cpp": transform_traversal_controller,
    "src/server/services/TraversalService.cpp": transform_traversal_service,
}


def main() -> int:
    originals = {rel: read(rel) for rel in HOT_FILES}
    transformed = {rel: TRANSFORMS[rel](originals[rel]) for rel in HOT_FILES}

    flat_rel = "include/utilities/FlatIdSet.hpp"
    flat_path = ROOT / flat_rel
    if flat_path.exists() and flat_path.read_text(encoding="utf-8") != FLAT_ID_SET:
        raise RuntimeError(
            f"{flat_rel} already exists with different contents; reconcile it first"
        )

    changed = []
    for rel, new_text in transformed.items():
        if new_text != originals[rel]:
            (ROOT / rel).write_text(new_text, encoding="utf-8")
            changed.append(rel)

    if not flat_path.exists():
        flat_path.parent.mkdir(parents=True, exist_ok=True)
        flat_path.write_text(FLAT_ID_SET, encoding="utf-8")
        changed.append(flat_rel)

    print("Low-memory BFS source edits applied successfully.")
    for rel in changed:
        print(f"  {rel}")
    print("\nNext:")
    print("  git diff --check")
    print('  cmake --build build -j"$(nproc)" --target polygraph_http_server')
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
