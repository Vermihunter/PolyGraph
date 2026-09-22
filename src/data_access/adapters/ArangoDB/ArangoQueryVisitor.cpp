/**
 * @file src/data_access/adapters/ArangoDB/ArangoQueryVisitor.cpp
 * @brief Implementation for arango query visitor in the graph-store access module.
 */

#include "data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <thread>

#include "events/Events.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "query/ResultParseType.hpp"
#include "query/query_types/GetNeighborsBatchQuery.hpp"
#include "query/query_types/GetNeighborsForVertexQuery.hpp"
#include "query/query_types/GetVertexByIdQuery.hpp"
#include "query/query_types/InsertBatchQuery.hpp"
#include "query/query_types/InsertQuery.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/Profiling.hpp"

#include <data_model/Record.hpp>
#include <mutex>
#include <optional>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <serialization/DataModelJsonSerializer.hpp>
#include <serialization/RecordJsonSerializer.hpp>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>
#include <utilities/uuid.hpp>

namespace polygraph {

    struct ArangoQueryVisitor::Impl final : QueryVisitor {
        const BackendConnectionConfig &config_;
        std::shared_ptr<BackendRequestScheduler> scheduler_;
        DataModelJsonSerializer serializer_;
        mutable CurlHttpClient http_;
        mutable std::mutex collection_cache_mutex_;
        mutable std::optional<std::unordered_set<std::string>> node_collection_cache_;
        mutable std::optional<std::unordered_set<std::string>> edge_collection_cache_;

        enum class collection_t : std::size_t { NODE = 2, EDGE = 3 };

    public:
        Impl(DatabaseContext &db_context, const BackendConnectionConfig &config,
             std::shared_ptr<BackendRequestScheduler> scheduler) :
            QueryVisitor(db_context), config_(config), scheduler_(std::move(scheduler)),
            http_(config.credentials.has_value() ? config.credentials->username + ":" + config.credentials->password
                                                 : "") {
            if (!scheduler_) {
                throw std::invalid_argument("ArangoQueryVisitor requires a backend request scheduler");
            }
        }

        query_result_t visit(const GetAllEdgesQuery &) override {
            return evaluate(construct_all_query(collection_t::EDGE), json::object(), ResultParseType::Relationship);
        }

        query_result_t visit(const GetAllVerticesQuery &) override {
            return evaluate(construct_all_query(collection_t::NODE), json::object(), ResultParseType::Node);
        }

        query_result_t visit(const GetNeighborsForVertexQuery &q) override {
            const Node &node = q.node();

            NodeRef ref{.global_id = node.global_id, .type = node.type};

            std::vector<NodeRef> frontier;
            frontier.push_back(ref);

            ExpandSpec spec;
            spec.direction = Direction::Both;

            return visit(GetNeighborsBatchQuery(std::move(frontier), spec));
        }

        query_result_t visit(const GetVertexByIdQuery &q) override {
            const Node &node = q.node();

            const std::string collection = db_context_.schema->node_type(node.type).name;

            const std::string query = "LET doc = DOCUMENT(@@collection, @key) "
                                      "FILTER doc != null RETURN doc";

            json bindVars = {{"@collection", collection}, {"key", global_id_to_string(node.global_id)}};

            return evaluate(query, bindVars, ResultParseType::Node);
        }

        query_result_t visit(const GetNeighborsBatchQuery &q) override {
            // POLYGRAPH_PROFILE_FUNCTION();

            EVENT_SCOPE_INFO(_, "arango.neighbor_query", {"frontier_size", q.frontier().size()});
            auto grouped_frontier = group_frontier_by_collection(q.frontier());

            auto combined = std::make_unique<QueryResult>();

            for (const auto &[collection, frontier]: grouped_frontier) {
                if (frontier.empty()) {
                    continue;
                }

                json frontierJson = json::array();

                for (const auto &v: frontier) {
                    frontierJson.push_back(global_id_to_string(v.global_id));
                }

                std::vector<TypeId> edge_type_ids = edge_types_for_query(q.spec());

                if (edge_type_ids.empty()) {
                    continue;
                }

                json bindVars = {{"@vertexCollection", collection}, {"frontier", frontierJson}};

                std::string edgeCollectionsAql;

                for (std::size_t i = 0; i < edge_type_ids.size(); ++i) {
                    const std::string bindName = "edgeCollection" + std::to_string(i);
                    const std::string bindKey = "@" + bindName;

                    bindVars[bindKey] = db_context_.schema->edge_type(edge_type_ids[i]).name;

                    if (i > 0) {
                        edgeCollectionsAql += ", ";
                    }

                    edgeCollectionsAql += "@@" + bindName;
                }

                const std::string direction = arango_direction(q.spec().direction);

                /*
                 * Result shape:
                 *
                 * {
                 *   source: start,
                 *   neighbor: neighbor,
                 *   edge: MERGE(edge, {
                 *      start: DOCUMENT(edge._from).uuid,
                 *      end: DOCUMENT(edge._to).uuid
                 *   })
                 * }
                 *
                 * DOCUMENT(edge._from).uuid and DOCUMENT(edge._to).uuid preserve
                 * the real stored edge direction even when traversal uses ANY.
                 */
                // NOLINTBEGIN(performance-inefficient-string-concatenation)
                const std::string query = "FOR source_key IN @frontier "
                                          "LET start = DOCUMENT(@@vertexCollection, source_key) "
                                          "FILTER start != null "
                                          "FOR neighbor, edge IN 1..1 " +
                                          direction + " start " + edgeCollectionsAql +
                                          " "
                                          "OPTIONS { uniqueVertices: \"none\" } "
                                          "LET edgeStart = DOCUMENT(edge._from) "
                                          "LET edgeEnd = DOCUMENT(edge._to) "
                                          "RETURN { "
                                          "  source: start, "
                                          "  neighbor: neighbor, "
                                          "  edge: MERGE(edge, { "
                                          "    start: edgeStart.uuid, "
                                          "    end: edgeEnd.uuid "
                                          "  }) "
                                          "}";
                // NOLINTEND(performance-inefficient-string-concatenation)

                auto partial = evaluate(query, bindVars, ResultParseType::AdjacencyRow);
                combined->combine(std::move(partial));
            }

            return combined;
        }

        std::vector<MinimalAdjacencyRow>
        get_minimal_neighbors_batch_fast(std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth,
                                         std::size_t batch_size, std::size_t max_concurrent_requests,
                                         ArangoRequestStrategy request_strategy,
                                         std::size_t single_request_max_frontier, std::size_t request_retries,
                                         std::size_t retry_delay_ms, const BackendRequestScheduling &scheduling) const {
            /*
             * Keep the switch here: one active implementation and the other
             * two preserved immediately below it for reproducible benchmarks.
             */

            // return get_minimal_neighbors_batch_edge_index_union(frontier, spec, depth);
            //  return get_minimal_neighbors_batch_traversal_projection(frontier, spec, depth);
            return get_minimal_neighbors_batch_edge_index_projection_async(
                    frontier, spec, depth, batch_size, max_concurrent_requests, request_strategy,
                    single_request_max_frontier, request_retries, retry_delay_ms, scheduling);
            // return get_minimal_neighbors_batch_original(frontier, spec, depth);
        }

        void stream_neighbor_candidates_fast(std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth,
                                             std::size_t batch_size, std::size_t max_concurrent_requests,
                                             ArangoRequestStrategy request_strategy,
                                             std::size_t single_request_max_frontier, std::size_t request_retries,
                                             std::size_t retry_delay_ms, std::size_t candidate_batch_size,
                                             const NeighborBatchConsumer &consumer,
                                             const BackendRequestScheduling &scheduling) const {
            if (!consumer || frontier.empty()) {
                return;
            }
            if (batch_size == 0 || max_concurrent_requests == 0 || candidate_batch_size == 0) {
                throw std::invalid_argument("ArangoDB streaming batch sizes and concurrency must be greater than zero");
            }

            EVENT_SCOPE_INFO(total_span, "arango.stream_neighbor_candidates", {"depth", depth},
                             {"frontier_size", frontier.size()}, {"batch_size", batch_size},
                             {"max_concurrent_requests", max_concurrent_requests},
                             {"request_strategy", std::string(to_string(request_strategy))},
                             {"single_request_max_frontier", single_request_max_frontier},
                             {"candidate_batch_size", candidate_batch_size},
                             {"worker_strategy", "lazy-request-bounded-thread-work-queue"});

            std::unordered_map<std::string, std::vector<NodeRef>> grouped_frontier;
            {
                EVENT_SCOPE_INFO(group_span, "arango.stream.group_frontier", {"frontier_size", frontier.size()});
                grouped_frontier = group_frontier_by_collection(frontier);
                group_span.add_end_field("source_collection_count", grouped_frontier.size());
            }

            std::unordered_set<std::string> materialized_edge_collections;
            {
                EVENT_SCOPE_INFO(collection_span, "arango.stream.edge_collection_discovery");
                materialized_edge_collections = available_edge_collections();
                collection_span.add_end_field("collection_count", materialized_edge_collections.size());
            }

            struct RequestDescriptor {
                std::string vertex_collection;
                std::string edge_collection;
                std::string target_type;
                const std::vector<NodeRef> *source_group{};
                std::size_t offset{};
                std::size_t count{};
                std::size_t batch_index{};
                std::size_t batch_count{};
                std::size_t edge_scan_index{};
                std::size_t edge_scan_count{};
                bool outbound{};
                bool single_frontier_request{};
            };

            std::vector<RequestDescriptor> descriptors;
            std::size_t selected_scan_count = 0;
            std::size_t single_frontier_group_count = 0;
            std::size_t chunked_frontier_group_count = 0;
            {
                EVENT_SCOPE_INFO(prepare_span, "arango.stream.prepare_request_descriptors",
                                 {"frontier_size", frontier.size()}, {"batch_size", batch_size});
                for (const auto &[vertex_collection, source_group]: grouped_frontier) {
                    if (source_group.empty()) {
                        continue;
                    }
                    const auto scans = edge_index_scans_for_source_collection(vertex_collection, spec,
                                                                              materialized_edge_collections);
                    selected_scan_count += scans.size();
                    if (scans.empty()) {
                        continue;
                    }

                    const bool use_single = request_strategy == ArangoRequestStrategy::Single ||
                                            (request_strategy == ArangoRequestStrategy::Auto &&
                                             source_group.size() <= single_request_max_frontier);
                    const std::size_t effective_batch_size = use_single ? source_group.size() : batch_size;
                    const std::size_t batch_count = 1 + ((source_group.size() - 1) / effective_batch_size);
                    if (use_single) {
                        ++single_frontier_group_count;
                    } else {
                        ++chunked_frontier_group_count;
                    }

                    descriptors.reserve(descriptors.size() + batch_count * scans.size());
                    for (std::size_t offset = 0, batch_index = 0; offset < source_group.size();
                         offset += effective_batch_size, ++batch_index) {
                        const std::size_t current_count = std::min(effective_batch_size, source_group.size() - offset);
                        for (std::size_t scan_index = 0; scan_index < scans.size(); ++scan_index) {
                            const auto &scan = scans[scan_index];
                            descriptors.push_back(RequestDescriptor{.vertex_collection = vertex_collection,
                                                                    .edge_collection = scan.edge_collection,
                                                                    .target_type = scan.target_type,
                                                                    .source_group = &source_group,
                                                                    .offset = offset,
                                                                    .count = current_count,
                                                                    .batch_index = batch_index,
                                                                    .batch_count = batch_count,
                                                                    .edge_scan_index = scan_index,
                                                                    .edge_scan_count = scans.size(),
                                                                    .outbound = scan.outbound,
                                                                    .single_frontier_request = use_single});
                        }
                    }
                }
                prepare_span.add_end_field("request_count", descriptors.size());
                prepare_span.add_end_field("schema_pruned_edge_scan_count", selected_scan_count);
                prepare_span.add_end_field("single_frontier_group_count", single_frontier_group_count);
                prepare_span.add_end_field("chunked_frontier_group_count", chunked_frontier_group_count);
                // Intentionally no request_bytes here: request bodies are built
                // lazily by workers and destroyed immediately after each request.
                prepare_span.add_end_field("pre_serialized_request_bytes", 0);
            }

            total_span.add_end_field("request_count", descriptors.size());
            total_span.add_end_field("schema_pruned_edge_scan_count", selected_scan_count);
            total_span.add_end_field("single_frontier_group_count", single_frontier_group_count);
            total_span.add_end_field("chunked_frontier_group_count", chunked_frontier_group_count);
            total_span.add_end_field("pre_serialized_request_bytes", 0);
            if (descriptors.empty()) {
                return;
            }

            const std::size_t worker_count =
                    std::min({max_concurrent_requests, descriptors.size(), scheduler_->config().max_in_flight});
            std::atomic_size_t next_request{0};
            std::atomic_size_t completed_requests{0};
            std::atomic_size_t response_rows{0};
            std::atomic_size_t request_bytes{0};
            std::atomic_bool stop_requested{false};
            std::vector<std::exception_ptr> worker_errors(worker_count);
            std::vector<std::thread> workers;
            workers.reserve(worker_count);

            const auto worker_body = [this, &descriptors, &next_request, &completed_requests, &response_rows,
                                      &request_bytes, &stop_requested, &worker_errors, spec, request_retries,
                                      retry_delay_ms, candidate_batch_size, consumer,
                                      scheduling](const std::size_t worker_index) {
                EVENT_SCOPE_INFO(worker_span, "arango.stream.worker", {"worker_index", worker_index});
                std::size_t local_completed = 0;
                std::size_t local_rows = 0;
                try {
                    auto http = make_http_client();
                    while (!stop_requested.load(std::memory_order_relaxed)) {
                        const std::size_t descriptor_index = next_request.fetch_add(1, std::memory_order_relaxed);
                        if (descriptor_index >= descriptors.size()) {
                            break;
                        }
                        const auto &descriptor = descriptors[descriptor_index];

                        EVENT_SCOPE_INFO(
                                request_span, "arango.stream.edge_index_query",
                                {"vertex_collection", descriptor.vertex_collection},
                                {"edge_collection", descriptor.edge_collection},
                                {"edge_direction", descriptor.outbound ? "outbound" : "inbound"},
                                {"group_size", descriptor.count}, {"batch_index", descriptor.batch_index},
                                {"batch_count", descriptor.batch_count},
                                {"edge_scan_index", descriptor.edge_scan_index},
                                {"edge_scan_count", descriptor.edge_scan_count},
                                {"request_strategy", descriptor.single_frontier_request ? "single" : "chunked"},
                                {"worker_index", worker_index}, {"prepared_index", descriptor_index});

                        for (std::size_t attempt = 0;; ++attempt) {
                            try {
                                std::size_t current_request_bytes = 0;
                                const std::size_t rows = scheduler_->run(scheduling, "arango.cursor", [&] {
                                    std::string request_body;
                                    {
                                        EVENT_SCOPE_INFO(construct_span, "arango.stream.request_construct",
                                                         {"group_size", descriptor.count},
                                                         {"edge_collection", descriptor.edge_collection});
                                        json frontier_json = json::array();
                                        auto &frontier_array = frontier_json.get_ref<json::array_t &>();
                                        frontier_array.reserve(descriptor.count);
                                        const auto &group = *descriptor.source_group;
                                        for (std::size_t i = 0; i < descriptor.count; ++i) {
                                            const NodeRef &source = group[descriptor.offset + i];
                                            frontier_array.emplace_back(descriptor.vertex_collection + "/" +
                                                                        global_id_to_string(source.global_id));
                                        }
                                        const json bind_vars{{"@edgeCollection", descriptor.edge_collection},
                                                             {"targetType", descriptor.target_type},
                                                             {"frontier", std::move(frontier_json)}};
                                        request_body = build_minimal_cursor_request_body(
                                                compact_edge_index_candidate_query(descriptor.outbound), bind_vars);
                                        current_request_bytes = request_body.size();
                                        construct_span.add_end_field("request_bytes", current_request_bytes);
                                    }
                                    return send_minimal_request_body_to_consumer(
                                            http, request_body, descriptor.vertex_collection, descriptor.count, spec,
                                            candidate_batch_size, consumer);
                                });
                                request_bytes.fetch_add(current_request_bytes, std::memory_order_relaxed);
                                ++local_completed;
                                local_rows += rows;
                                completed_requests.fetch_add(1, std::memory_order_relaxed);
                                response_rows.fetch_add(rows, std::memory_order_relaxed);
                                request_span.add_end_field("attempts", attempt + 1);
                                request_span.add_end_field("request_bytes", current_request_bytes);
                                request_span.add_end_field("response_row_count", rows);
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
                                        retry_delay_ms * (std::size_t{1} << std::min(attempt, std::size_t{10}));
                                const std::size_t jitter = retry_delay_ms == 0 ? 0 : (worker_index % 8) * 7;
                                std::this_thread::sleep_for(std::chrono::milliseconds(delay + jitter));
                            }
                        }
                    }
                    worker_span.add_end_field("completed_request_count", local_completed);
                    worker_span.add_end_field("response_row_count", local_rows);
                    worker_span.add_end_field("status", "ok");
                } catch (...) {
                    stop_requested.store(true, std::memory_order_relaxed);
                    worker_errors[worker_index] = std::current_exception();
                    worker_span.add_end_field("completed_request_count", local_completed);
                    worker_span.add_end_field("response_row_count", local_rows);
                    worker_span.add_end_field("status", "failed");
                }
            };

            {
                EVENT_SCOPE_INFO(execution_span, "arango.stream.worker_execution", {"worker_count", worker_count},
                                 {"prepared_request_count", descriptors.size()});
                for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
                    workers.emplace_back(worker_body, worker_index);
                }
                for (auto &worker: workers) {
                    worker.join();
                }
            }

            for (const auto &error: worker_errors) {
                if (error) {
                    total_span.add_end_field("status", "failed");
                    std::rethrow_exception(error);
                }
            }

            total_span.add_end_field("completed_request_count", completed_requests.load(std::memory_order_relaxed));
            total_span.add_end_field("response_row_count", response_rows.load(std::memory_order_relaxed));
            total_span.add_end_field("request_bytes", request_bytes.load(std::memory_order_relaxed));
            total_span.add_end_field("worker_count", worker_count);
            total_span.add_end_field("status", "ok");
        }

        query_result_t visit(const InsertQuery &query) override {
            std::vector<insertion::InsertionItem> items;
            items.push_back(query.item());
            return visit(InsertBatchQuery(std::move(items)));
        }

        query_result_t visit(const InsertBatchQuery &query) override {
            if (query.empty()) {
                return std::make_unique<QueryResult>();
            }

            std::unordered_map<TypeId, json> node_groups;
            std::unordered_map<TypeId, json> edge_groups;
            for (const insertion::InsertionItem &item: query.items()) {
                if (item.is_node()) {
                    json &rows = node_groups[item.type()];
                    if (!rows.is_array()) {
                        rows = json::array();
                    }
                    rows.push_back(node_to_write_row(item.node()));
                } else {
                    json &rows = edge_groups[item.type()];
                    if (!rows.is_array()) {
                        rows = json::array();
                    }
                    rows.push_back(edge_to_write_row(item.edge()));
                }
            }

            for (const auto &[node_type, rows]: node_groups) {
                const std::string query_text =
                        "LET inserted = ("
                        "  FOR row IN @rows "
                        "    INSERT MERGE(row.properties, {"
                        "      _key: row.key, uuid: row.uuid, polygraph_type_id: row.type, labels: row.labels"
                        "    }) INTO @@collection "
                        "    RETURN 1"
                        ") RETURN LENGTH(inserted)";
                const json bind_vars{{"rows", rows}, {"@collection", db_context_.schema->node_type(node_type).name}};
                execute_write(query_text, bind_vars, rows.size());
            }

            for (const auto &[edge_type, rows]: edge_groups) {
                const auto &schema_edge = db_context_.schema->edge_type(edge_type);
                const std::string query_text = "LET inserted = ("
                                               "  FOR row IN @rows "
                                               "    LET source = DOCUMENT(@@sourceCollection, row.src_key) "
                                               "    LET target = DOCUMENT(@@targetCollection, row.dst_key) "
                                               "    FILTER source != null AND target != null "
                                               "    INSERT MERGE(row.properties, {"
                                               "      _key: row.key, uuid: row.uuid, polygraph_type_id: row.type,"
                                               "      _from: source._id, _to: target._id"
                                               "    }) INTO @@edgeCollection "
                                               "    RETURN 1"
                                               ") RETURN LENGTH(inserted)";
                const json bind_vars{{"rows", rows},
                                     {"@sourceCollection", db_context_.schema->node_type(schema_edge.from).name},
                                     {"@targetCollection", db_context_.schema->node_type(schema_edge.to).name},
                                     {"@edgeCollection", schema_edge.name}};
                execute_write(query_text, bind_vars, rows.size());
            }

            return std::make_unique<QueryResult>();
        }

    private:
        static GlobalId json_global_id(const json &value) {
            if (value.is_number_unsigned()) {
                return value.get<GlobalId>();
            }
            if (value.is_number_integer()) {
                return global_id_from_signed_storage(value.get<std::int64_t>());
            }
            if (value.is_string()) {
                return global_id_from_string(value.get<std::string>());
            }
            throw std::runtime_error("ArangoDB global ID must be an integer or string: " + value.dump());
        }

        static GlobalId global_id_from_document_id(const std::string &document_id) {
            const auto slash = document_id.rfind('/');
            if (slash == std::string::npos || slash + 1 >= document_id.size()) {
                throw std::runtime_error("Invalid Arango document id: " + document_id);
            }
            return global_id_from_string(document_id.substr(slash + 1));
        }

        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_edge_index_union(std::span<const NodeRef> frontier,
                                                                                      const ExpandSpec &spec,
                                                                                      int depth) const {
            (void) depth;

            if (frontier.empty()) {
                return {};
            }

            EVENT_SCOPE_INFO(total_span, "arango.minimal_neighbors.edge_index_union",
                             {"frontier_size", frontier.size()});

            auto [rows, row_by_source] = initialize_minimal_rows(frontier);
            const auto grouped_frontier = group_frontier_by_collection(frontier);
            const auto edge_type_ids = edge_types_for_query(spec);

            for (const auto &[vertex_collection, source_group]: grouped_frontier) {
                if (source_group.empty() || edge_type_ids.empty()) {
                    continue;
                }

                json frontier_json = json::array();
                frontier_json.get_ref<json::array_t &>().reserve(source_group.size());

                for (const auto &source: source_group) {
                    frontier_json.push_back(global_id_to_string(source.global_id));
                }

                json bind_vars{{"@vertexCollection", vertex_collection}, {"frontier", std::move(frontier_json)}};
                std::vector<std::string> subqueries;
                subqueries.reserve(edge_type_ids.size() * (spec.direction == Direction::Directed ? 1U : 2U));

                for (std::size_t edge_index = 0; edge_index < edge_type_ids.size(); ++edge_index) {
                    const std::string bind_name = "edgeCollection" + std::to_string(edge_index);
                    bind_vars["@" + bind_name] = db_context_.schema->edge_type(edge_type_ids[edge_index]).name;

                    subqueries.push_back(build_edge_index_subquery(bind_name, true));

                    if (spec.direction != Direction::Directed) {
                        subqueries.push_back(build_edge_index_subquery(bind_name, false));
                    }
                }

                std::string query = "LET starts = ("
                                    "  FOR source_key IN @frontier "
                                    "    LET start = DOCUMENT(@@vertexCollection, source_key) "
                                    "    FILTER start != null "
                                    "    RETURN { id: start._id, uuid: start.uuid }"
                                    ") "
                                    "RETURN UNION(";

                for (std::size_t index = 0; index < subqueries.size(); ++index) {
                    if (index > 0) {
                        query += ", ";
                    }

                    query += subqueries[index];
                }

                query += ")";

                const json response = send_req(query, bind_vars);
                parse_minimal_response_into_rows(response, rows, row_by_source, spec);
            }

            total_span.add_end_field("result_source_rows", rows.size());
            return rows;
        }

        std::vector<MinimalAdjacencyRow>
        get_minimal_neighbors_batch_traversal_projection(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                         int depth) const {
            (void) depth;

            if (frontier.empty()) {
                return {};
            }

            EVENT_SCOPE_INFO(total_span, "arango.minimal_neighbors.traversal_projection",
                             {"frontier_size", frontier.size()});

            auto [rows, row_by_source] = initialize_minimal_rows(frontier);
            const auto grouped_frontier = group_frontier_by_collection(frontier);
            const auto edge_type_ids = edge_types_for_query(spec);

            for (const auto &[vertex_collection, source_group]: grouped_frontier) {
                if (source_group.empty() || edge_type_ids.empty()) {
                    continue;
                }

                json frontier_json = json::array();
                frontier_json.get_ref<json::array_t &>().reserve(source_group.size());

                for (const auto &source: source_group) {
                    frontier_json.push_back(global_id_to_string(source.global_id));
                }

                json bind_vars{{"@vertexCollection", vertex_collection}, {"frontier", std::move(frontier_json)}};
                std::string edge_collections;

                for (std::size_t edge_index = 0; edge_index < edge_type_ids.size(); ++edge_index) {
                    const std::string bind_name = "edgeCollection" + std::to_string(edge_index);
                    bind_vars["@" + bind_name] = db_context_.schema->edge_type(edge_type_ids[edge_index]).name;

                    if (!edge_collections.empty()) {
                        edge_collections += ", ";
                    }

                    edge_collections += "@@" + bind_name;
                }

                const std::string query = "FOR source_key IN @frontier "
                                          "LET start = DOCUMENT(@@vertexCollection, source_key) "
                                          "FILTER start != null "
                                          "FOR neighbor IN 1..1 " +
                                          arango_direction(spec.direction) + " start " + edge_collections +
                                          " OPTIONS { uniqueVertices: \"none\" } "
                                          "RETURN ["
                                          "  start.uuid, "
                                          "  neighbor.uuid, "
                                          "  neighbor._id, "
                                          "  neighbor[\"remote-type\"]"
                                          "]";

                const json response = send_req(query, bind_vars);
                parse_minimal_response_into_rows(response, rows, row_by_source, spec);
            }

            total_span.add_end_field("result_source_rows", rows.size());
            return rows;
        }

        struct ArangoEdgeIndexScan {
            std::string edge_collection;
            bool outbound{};
            std::string target_type;
        };

        [[nodiscard]] std::vector<ArangoEdgeIndexScan> edge_index_scans_for_source_collection(
                const std::string &source_collection, const ExpandSpec &spec,
                const std::unordered_set<std::string> &available_edge_collections) const {
            const TypeId source_type = db_context_.schema->node_type_id(source_collection);
            std::vector<ArangoEdgeIndexScan> scans;

            const auto append_scans = [&](const TypeId edge_type_id) {
                const auto &edge_type = db_context_.schema->edge_type(edge_type_id);
                if (!available_edge_collections.contains(edge_type.name)) {
                    return;
                }

                if (edge_type.from == source_type && spec.allows_target_type(edge_type.to)) {
                    scans.push_back(
                            ArangoEdgeIndexScan{.edge_collection = edge_type.name,
                                                .outbound = true,
                                                .target_type = db_context_.schema->node_type(edge_type.to).name});
                }

                if (spec.direction != Direction::Directed && edge_type.to == source_type &&
                    spec.allows_target_type(edge_type.from)) {
                    scans.push_back(
                            ArangoEdgeIndexScan{.edge_collection = edge_type.name,
                                                .outbound = false,
                                                .target_type = db_context_.schema->node_type(edge_type.from).name});
                }
            };

            if (!spec.allowed_edge_types.empty()) {
                scans.reserve(spec.allowed_edge_types.size() * (spec.direction == Direction::Directed ? 1U : 2U));
                for (const TypeId edge_type_id: spec.allowed_edge_types) {
                    append_scans(edge_type_id);
                }
            } else {
                scans.reserve(db_context_.schema->edge_types().size() *
                              (spec.direction == Direction::Directed ? 1U : 2U));
                for (const auto &[edge_type_id, edge_type]: db_context_.schema->edge_types()) {
                    (void) edge_type;
                    append_scans(edge_type_id);
                }
            }

            return scans;
        }

        [[nodiscard]] static std::string compact_edge_index_query(const bool outbound) {
            const std::string source_endpoint = outbound ? "edge._from" : "edge._to";
            const std::string neighbor_endpoint = outbound ? "edge._to" : "edge._from";

            // One edge collection and one direction per cursor request.  Keeping the
            // lookup this small is deliberate: the old fast path wrapped all matching
            // edge scans in AQL UNION(...), which can materialize large intermediate
            // arrays inside ArangoDB before the cursor can stream them.  This query is
            // just a batched endpoint-index lookup and PolyGraph merges the independent
            // streams on the client side.
            return "FOR edge IN @@edgeCollection "
                   "FILTER " +
                   source_endpoint +
                   " IN @frontier "
                   "RETURN [" +
                   source_endpoint + ", " + neighbor_endpoint + ", @targetType]";
        }

        [[nodiscard]] static std::string compact_edge_index_candidate_query(const bool outbound) {
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

        [[nodiscard]] static std::string build_minimal_cursor_request_body(const std::string &query,
                                                                           const json &bind_vars) {
            // Keep the server cursor bounded. The response parser consumes rows while
            // parsing, so the client does not retain a second full JSON result tree.
            return json{{"query", query}, {"bindVars", bind_vars}, {"batchSize", 50000}, {"cache", false}}.dump();
        }

        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_edge_index_projection_async(
                std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth, std::size_t batch_size,
                std::size_t max_concurrent_requests, ArangoRequestStrategy request_strategy,
                std::size_t single_request_max_frontier, std::size_t request_retries, std::size_t retry_delay_ms,
                const BackendRequestScheduling &scheduling) const {
            (void) depth;

            if (frontier.empty()) {
                return {};
            }
            if (batch_size == 0) {
                throw std::invalid_argument("ArangoDB neighbor batch size must be greater than zero");
            }
            if (max_concurrent_requests == 0) {
                throw std::invalid_argument("ArangoDB max concurrent requests must be greater than zero");
            }

            EVENT_SCOPE_INFO(total_span, "arango.minimal_neighbors.edge_index_bounded",
                             {"frontier_size", frontier.size()}, {"batch_size", batch_size},
                             {"max_concurrent_requests", max_concurrent_requests}, {"request_retries", request_retries},
                             {"request_strategy", std::string(to_string(request_strategy))},
                             {"single_request_max_frontier", single_request_max_frontier},
                             {"query_strategy", "single-edge-index-requests"},
                             {"worker_strategy", "bounded-thread-work-queue"});

            auto [rows, row_by_source] = initialize_minimal_rows(frontier);

            std::unordered_map<std::string, std::vector<NodeRef>> grouped_frontier;
            {
                EVENT_SCOPE_INFO(group_span, "arango.minimal_neighbors.group_frontier",
                                 {"frontier_size", frontier.size()});
                grouped_frontier = group_frontier_by_collection(frontier);
                group_span.add_end_field("source_collection_count", grouped_frontier.size());
            }

            std::unordered_set<std::string> materialized_edge_collections;
            {
                EVENT_SCOPE_INFO(collection_span, "arango.minimal_neighbors.edge_collection_discovery");
                materialized_edge_collections = available_edge_collections();
                collection_span.add_end_field("collection_count", materialized_edge_collections.size());
            }

            struct ArangoPreparedBatch {
                std::string vertex_collection;
                std::string edge_collection;
                std::size_t batch_index{};
                std::size_t batch_count{};
                std::size_t edge_scan_index{};
                std::size_t edge_scan_count{};
                bool outbound{};
                bool single_frontier_request{};
                std::shared_ptr<const std::vector<NodeRef>> nodes;
                std::string request_body;
            };

            struct ArangoBatchResult {
                std::size_t completed_requests{};
                std::size_t response_row_count{};
                std::vector<MinimalAdjacencyRow> rows;
            };

            std::vector<ArangoPreparedBatch> prepared_batches;
            std::size_t unpruned_scan_count = 0;
            std::size_t selected_scan_count = 0;
            std::size_t prepared_request_bytes = 0;
            std::size_t single_frontier_group_count = 0;
            std::size_t chunked_frontier_group_count = 0;

            {
                EVENT_SCOPE_INFO(prepare_span, "arango.minimal_neighbors.prepare_requests",
                                 {"frontier_size", frontier.size()}, {"batch_size", batch_size},
                                 {"source_collection_count", grouped_frontier.size()});

                for (const auto &[vertex_collection, source_group]: grouped_frontier) {
                    if (source_group.empty()) {
                        continue;
                    }

                    const auto scans = edge_index_scans_for_source_collection(vertex_collection, spec,
                                                                              materialized_edge_collections);
                    unpruned_scan_count +=
                            materialized_edge_collections.size() * (spec.direction == Direction::Directed ? 1U : 2U);
                    selected_scan_count += scans.size();
                    if (scans.empty()) {
                        continue;
                    }

                    const bool use_single_frontier_request = request_strategy == ArangoRequestStrategy::Single ||
                                                             (request_strategy == ArangoRequestStrategy::Auto &&
                                                              source_group.size() <= single_request_max_frontier);
                    const std::size_t effective_batch_size =
                            use_single_frontier_request ? source_group.size() : batch_size;
                    const std::size_t batch_count = 1 + ((source_group.size() - 1) / effective_batch_size);
                    if (use_single_frontier_request) {
                        ++single_frontier_group_count;
                    } else {
                        ++chunked_frontier_group_count;
                    }
                    prepared_batches.reserve(prepared_batches.size() + batch_count * scans.size());

                    for (std::size_t offset = 0, batch_index = 0; offset < source_group.size();
                         offset += effective_batch_size, ++batch_index) {
                        const std::size_t current_batch_size =
                                std::min(effective_batch_size, source_group.size() - offset);
                        auto batch_nodes = std::make_shared<const std::vector<NodeRef>>(
                                source_group.begin() + static_cast<std::ptrdiff_t>(offset),
                                source_group.begin() + static_cast<std::ptrdiff_t>(offset + current_batch_size));

                        json frontier_json = json::array();
                        auto &frontier_array = frontier_json.get_ref<json::array_t &>();
                        frontier_array.reserve(current_batch_size);
                        for (const NodeRef &source: *batch_nodes) {
                            frontier_array.emplace_back(vertex_collection + "/" +
                                                        global_id_to_string(source.global_id));
                        }

                        for (std::size_t scan_index = 0; scan_index < scans.size(); ++scan_index) {
                            const auto &scan = scans[scan_index];
                            json bind_vars{{"@edgeCollection", scan.edge_collection},
                                           {"targetType", scan.target_type},
                                           {"frontier", frontier_json}};
                            std::string request_body = build_minimal_cursor_request_body(
                                    compact_edge_index_query(scan.outbound), bind_vars);
                            prepared_request_bytes += request_body.size();

                            prepared_batches.push_back(
                                    ArangoPreparedBatch{.vertex_collection = vertex_collection,
                                                        .edge_collection = scan.edge_collection,
                                                        .batch_index = batch_index,
                                                        .batch_count = batch_count,
                                                        .edge_scan_index = scan_index,
                                                        .edge_scan_count = scans.size(),
                                                        .outbound = scan.outbound,
                                                        .single_frontier_request = use_single_frontier_request,
                                                        .nodes = batch_nodes,
                                                        .request_body = std::move(request_body)});
                        }
                    }
                }

                prepare_span.add_end_field("request_count", prepared_batches.size());
                prepare_span.add_end_field("request_bytes", prepared_request_bytes);
                prepare_span.add_end_field("single_frontier_group_count", single_frontier_group_count);
                prepare_span.add_end_field("chunked_frontier_group_count", chunked_frontier_group_count);
                prepare_span.add_end_field("schema_pruned_edge_scan_count", selected_scan_count);
                prepare_span.add_end_field("unpruned_edge_scan_upper_bound", unpruned_scan_count);
            }

            total_span.add_end_field("request_count", prepared_batches.size());
            total_span.add_end_field("request_bytes", prepared_request_bytes);
            total_span.add_end_field("single_frontier_group_count", single_frontier_group_count);
            total_span.add_end_field("chunked_frontier_group_count", chunked_frontier_group_count);
            total_span.add_end_field("schema_pruned_edge_scan_count", selected_scan_count);
            total_span.add_end_field("unpruned_edge_scan_upper_bound", unpruned_scan_count);

            if (prepared_batches.empty()) {
                total_span.add_end_field("completed_request_count", 0);
                total_span.add_end_field("response_row_count", 0);
                total_span.add_end_field("result_source_rows", rows.size());
                return rows;
            }

            const std::size_t worker_count =
                    std::min({max_concurrent_requests, prepared_batches.size(), scheduler_->config().max_in_flight});
            std::atomic_size_t next_batch{0};
            std::atomic_bool stop_requested{false};
            std::vector<ArangoBatchResult> worker_results(worker_count);
            std::vector<std::exception_ptr> worker_errors(worker_count);
            std::vector<std::thread> workers;
            workers.reserve(worker_count);

            // The old implementation used std::async plus a workers_ready/start_gate
            // barrier so that the first request from every worker was released at once.
            // On some runtimes that barrier could take minutes before all async tasks
            // were actually scheduled.  A bounded set of real threads consuming one
            // atomic work queue has the concurrency semantics we need without a launch
            // barrier: as soon as a worker exists it can issue useful I/O.
            const auto dispatch_started = std::chrono::steady_clock::now();
            const auto worker_body = [this, &prepared_batches, &next_batch, &stop_requested, &worker_results,
                                      &worker_errors, dispatch_started, spec, request_retries, retry_delay_ms,
                                      scheduling](const std::size_t worker_index) mutable {
                EVENT_SCOPE_INFO(worker_span, "arango.minimal_neighbors.worker", {"worker_index", worker_index});

                auto &stats = worker_results[worker_index];
                std::size_t worker_request_count = 0;
                try {
                    auto http = make_http_client();

                    const auto execute_batch = [&](const std::size_t prepared_index) {
                        const auto &prepared = prepared_batches[prepared_index];
                        const std::size_t current_batch_size = prepared.nodes->size();
                        const auto queue_wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                           std::chrono::steady_clock::now() - dispatch_started)
                                                           .count();
                        EVENT_SCOPE_INFO(
                                request_span, "arango.minimal_neighbors.edge_index_query",
                                {"vertex_collection", prepared.vertex_collection},
                                {"edge_collection", prepared.edge_collection},
                                {"edge_direction", prepared.outbound ? "outbound" : "inbound"},
                                {"request_strategy", prepared.single_frontier_request ? "single" : "chunked"},
                                {"group_size", current_batch_size}, {"batch_index", prepared.batch_index},
                                {"batch_count", prepared.batch_count}, {"edge_scan_index", prepared.edge_scan_index},
                                {"edge_scan_count", prepared.edge_scan_count},
                                {"request_bytes", prepared.request_body.size()}, {"worker_index", worker_index},
                                {"prepared_index", prepared_index}, {"worker_request_index", worker_request_count},
                                {"queue_wait_ns", queue_wait_ns});

                        for (std::size_t attempt = 0;; ++attempt) {
                            try {
                                auto [batch_rows, batch_row_by_source] = initialize_minimal_rows(*prepared.nodes);
                                const std::size_t response_rows = scheduler_->run(scheduling, "arango.cursor", [&] {
                                    return send_minimal_request_body_into_rows(
                                            http, prepared.request_body, prepared.vertex_collection, current_batch_size,
                                            batch_rows, batch_row_by_source, spec);
                                });

                                stats.rows.insert(stats.rows.end(), std::make_move_iterator(batch_rows.begin()),
                                                  std::make_move_iterator(batch_rows.end()));
                                ++stats.completed_requests;
                                stats.response_row_count += response_rows;
                                ++worker_request_count;
                                request_span.add_end_field("attempts", attempt + 1);
                                request_span.add_end_field("response_row_count", response_rows);
                                request_span.add_end_field("status", "ok");
                                return;
                            } catch (const std::exception &error) {
                                if (attempt >= request_retries) {
                                    request_span.add_end_field("attempts", attempt + 1);
                                    request_span.add_end_field("status", "failed");
                                    request_span.add_end_field("error", error.what());
                                    throw std::runtime_error(
                                            "ArangoDB neighbor batch " + std::to_string(prepared.batch_index + 1) +
                                            "/" + std::to_string(prepared.batch_count) + " for collection '" +
                                            prepared.vertex_collection + "' failed after " +
                                            std::to_string(attempt + 1) + " attempt(s): " + error.what());
                                }

                                // Preserve connection reuse on the happy path, but discard
                                // a handle after a failed transfer because its socket may be bad.
                                http = make_http_client();
                                const std::size_t delay =
                                        retry_delay_ms * (std::size_t{1} << std::min(attempt, std::size_t{10}));
                                const std::size_t jitter = retry_delay_ms == 0 ? 0 : (worker_index % 8) * 7;
                                EVENT_SCOPE_INFO(retry_span, "arango.minimal_neighbors.request_retry",
                                                 {"vertex_collection", prepared.vertex_collection},
                                                 {"batch_index", prepared.batch_index}, {"attempt", attempt + 1},
                                                 {"delay_ms", delay + jitter}, {"error", error.what()});
                                std::this_thread::sleep_for(std::chrono::milliseconds(delay + jitter));
                            }
                        }
                    };

                    while (!stop_requested.load(std::memory_order_relaxed)) {
                        const std::size_t prepared_index = next_batch.fetch_add(1, std::memory_order_relaxed);
                        if (prepared_index >= prepared_batches.size()) {
                            break;
                        }
                        execute_batch(prepared_index);
                    }

                    worker_span.add_end_field("request_count", worker_request_count);
                    worker_span.add_end_field("completed_request_count", stats.completed_requests);
                    worker_span.add_end_field("response_row_count", stats.response_row_count);
                    worker_span.add_end_field("status", "ok");
                } catch (...) {
                    // Stop handing out new work once one worker has failed. Requests
                    // already in flight are allowed to finish before the join below.
                    stop_requested.store(true, std::memory_order_relaxed);
                    worker_errors[worker_index] = std::current_exception();
                    worker_span.add_end_field("request_count", worker_request_count);
                    worker_span.add_end_field("completed_request_count", stats.completed_requests);
                    worker_span.add_end_field("response_row_count", stats.response_row_count);
                    worker_span.add_end_field("status", "failed");
                }
            };

            {
                EVENT_SCOPE_INFO(worker_execution_span, "arango.minimal_neighbors.worker_execution",
                                 {"worker_count", worker_count}, {"prepared_request_count", prepared_batches.size()});

                {
                    EVENT_SCOPE_INFO(worker_creation_span, "arango.minimal_neighbors.worker_creation",
                                     {"worker_count", worker_count});
                    try {
                        for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
                            workers.emplace_back(worker_body, worker_index);
                        }
                    } catch (...) {
                        // std::thread construction itself can fail. Stop any successfully
                        // created workers and join them before propagating the error.
                        stop_requested.store(true, std::memory_order_relaxed);
                        for (auto &worker: workers) {
                            if (worker.joinable()) {
                                worker.join();
                            }
                        }
                        worker_creation_span.add_end_field("created_worker_count", workers.size());
                        worker_creation_span.add_end_field("status", "failed");
                        throw;
                    }
                    worker_creation_span.add_end_field("created_worker_count", workers.size());
                    worker_creation_span.add_end_field("status", "ok");
                }

                for (auto &worker: workers) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                }
                worker_execution_span.add_end_field("joined_worker_count", workers.size());
                worker_execution_span.add_end_field(
                        "claimed_request_count",
                        std::min(next_batch.load(std::memory_order_relaxed), prepared_batches.size()));
            }

            for (const auto &worker_error: worker_errors) {
                if (worker_error != nullptr) {
                    total_span.add_end_field("status", "failed");
                    std::rethrow_exception(worker_error);
                }
            }

            std::size_t completed_requests = 0;
            std::size_t total_response_rows = 0;
            {
                EVENT_SCOPE_INFO(merge_span, "arango.minimal_neighbors.merge_results", {"worker_count", worker_count});
                for (auto &result: worker_results) {
                    completed_requests += result.completed_requests;
                    total_response_rows += result.response_row_count;
                    merge_minimal_rows(std::move(result.rows), rows, row_by_source);
                }
                merge_span.add_end_field("completed_request_count", completed_requests);
                merge_span.add_end_field("response_row_count", total_response_rows);
                merge_span.add_end_field("result_source_rows", rows.size());
            }

            total_span.add_end_field("completed_request_count", completed_requests);
            total_span.add_end_field("response_row_count", total_response_rows);
            total_span.add_end_field("worker_count", worker_count);
            total_span.add_end_field("result_source_rows", rows.size());
            total_span.add_end_field("status", "ok");
            return rows;
        }

        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_original(std::span<const NodeRef> frontier,
                                                                              const ExpandSpec &spec, int depth) const {
            (void) depth;

            if (frontier.empty()) {
                return {};
            }

            auto [rows, row_by_source] = initialize_minimal_rows(frontier);
            auto raw = const_cast<Impl *>(this)->visit(
                    GetNeighborsBatchQuery(std::vector<NodeRef>(frontier.begin(), frontier.end()), spec));

            if (!raw) {
                return rows;
            }

            std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                    seen_by_source;

            for (const auto &record: raw->data) {
                if (!record || record->fields.size() < 2 || !record->fields[0].is<Node>() ||
                    !record->fields[1].is<Node>()) {
                    continue;
                }

                const auto &source_node = record->fields[0].as<Node>();
                const auto &neighbor_node = record->fields[1].as<Node>();
                const auto source_iterator = row_by_source.find(source_node.global_id);

                if (source_iterator == row_by_source.end() || !spec.allows_target_type(neighbor_node.type) ||
                    !seen_by_source[source_node.global_id].insert(neighbor_node.global_id).second) {
                    continue;
                }

                rows[source_iterator->second].neighbors.push_back(
                        NodeRef{.global_id = neighbor_node.global_id, .type = neighbor_node.type});
            }

            return rows;
        }

        static std::string build_edge_index_subquery(const std::string &edge_collection_bind_name, bool outbound) {
            const std::string edge_endpoint = outbound ? "edge._from" : "edge._to";
            const std::string neighbor_endpoint = outbound ? "edge._to" : "edge._from";

            return "(FOR start IN starts "
                   "   FOR edge IN @@" +
                   edge_collection_bind_name +
                   " "
                   "     FILTER " +
                   edge_endpoint +
                   " == start.id "
                   "     LET neighbor = DOCUMENT(" +
                   neighbor_endpoint +
                   ") "
                   "     FILTER neighbor != null "
                   "     RETURN ["
                   "       start.uuid, "
                   "       neighbor.uuid, "
                   "       neighbor._id, "
                   "       neighbor[\"remote-type\"]"
                   "     ])";
        }

        static std::pair<std::vector<MinimalAdjacencyRow>,
                         std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>>>
        initialize_minimal_rows(std::span<const NodeRef> frontier) {
            std::vector<MinimalAdjacencyRow> rows;
            rows.reserve(frontier.size());

            std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> row_by_source;
            row_by_source.reserve(frontier.size());

            for (const auto &source: frontier) {
                const auto [iterator, inserted] = row_by_source.emplace(source.global_id, rows.size());
                (void) iterator;

                if (inserted) {
                    rows.push_back(MinimalAdjacencyRow{.source = source, .neighbors = {}});
                }
            }

            return {std::move(rows), std::move(row_by_source)};
        }

        static void
        merge_minimal_rows(std::vector<MinimalAdjacencyRow> source_rows, std::vector<MinimalAdjacencyRow> &target_rows,
                           const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &target_row_by_source) {
            for (auto &source_row: source_rows) {
                const auto target_iterator = target_row_by_source.find(source_row.source.global_id);

                if (target_iterator == target_row_by_source.end()) {
                    continue;
                }

                auto &target_neighbors = target_rows[target_iterator->second].neighbors;
                target_neighbors.insert(target_neighbors.end(), std::make_move_iterator(source_row.neighbors.begin()),
                                        std::make_move_iterator(source_row.neighbors.end()));
            }
        }

        void parse_minimal_response_into_rows(
                const json &response, std::vector<MinimalAdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                const ExpandSpec &spec) const {
            if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                throw std::runtime_error("ArangoDB minimal neighbor query failed: " + response.dump());
            }

            if (!response.contains("result") || !response["result"].is_array()) {
                throw std::runtime_error("ArangoDB minimal neighbor response has no result array: " + response.dump());
            }

            std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                    seen_by_source;

            for (const auto &item: response["result"]) {
                append_minimal_result_item(item, rows, row_by_source, seen_by_source, spec);
            }
        }

        [[nodiscard]] static bool is_minimal_result_row(const json &item) {
            if (!item.is_array()) {
                return false;
            }

            const bool compact_edge_index_row = item.size() >= 3 && item[0].is_string() && item[1].is_string() &&
                                                item[2].is_string() &&
                                                item[0].get_ref<const std::string &>().find('/') != std::string::npos &&
                                                item[1].get_ref<const std::string &>().find('/') != std::string::npos;
            const bool legacy_projection_row =
                    item.size() >= 4 && (item[0].is_number_integer() || item[0].is_string()) &&
                    (item[1].is_number_integer() || item[1].is_string()) && item[2].is_string();
            return compact_edge_index_row || legacy_projection_row;
        }

        void append_minimal_result_item(
                const json &item, std::vector<MinimalAdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                        &seen_by_source,
                const ExpandSpec &spec) const {
            if (!item.is_array()) {
                return;
            }

            if (!is_minimal_result_row(item)) {
                for (const auto &nested: item) {
                    append_minimal_result_item(nested, rows, row_by_source, seen_by_source, spec);
                }
                return;
            }

            GlobalId source_id{};
            GlobalId neighbor_id{};
            std::string neighbor_document_id;
            std::string logical_type_name;

            const bool compact_edge_index_row = item.size() >= 3 && item[0].is_string() && item[1].is_string() &&
                                                item[2].is_string() &&
                                                item[0].get_ref<const std::string &>().find('/') != std::string::npos &&
                                                item[1].get_ref<const std::string &>().find('/') != std::string::npos;
            if (compact_edge_index_row) {
                source_id = global_id_from_document_id(item[0].get<std::string>());
                neighbor_document_id = item[1].get<std::string>();
                neighbor_id = global_id_from_document_id(neighbor_document_id);
                logical_type_name = item[2].get<std::string>();
            } else {
                source_id = json_global_id(item[0]);
                neighbor_id = json_global_id(item[1]);
                neighbor_document_id = item[2].get<std::string>();
                logical_type_name = collection_from_arango_id(neighbor_document_id);

                if (logical_type_name == "Proxy") {
                    if (item[3].is_null() || !item[3].is_string() || item[3].get<std::string>().empty()) {
                        throw std::runtime_error("ArangoDB proxy neighbor is missing remote-type: " + item.dump());
                    }
                    logical_type_name = item[3].get<std::string>();
                }
            }

            const auto source_iterator = row_by_source.find(source_id);
            if (source_iterator == row_by_source.end()) {
                return;
            }

            const TypeId neighbor_type = db_context_.schema->node_type_id(logical_type_name);
            if (!spec.allows_target_type(neighbor_type) || !seen_by_source[source_id].insert(neighbor_id).second) {
                return;
            }

            rows[source_iterator->second].neighbors.push_back(NodeRef{.global_id = neighbor_id, .type = neighbor_type});
        }

        std::unordered_map<std::string, std::vector<NodeRef>>
        group_frontier_by_collection(std::span<const NodeRef> frontier) const {
            std::unordered_map<std::string, std::vector<NodeRef>> grouped;
            std::vector<NodeRef> unresolved;

            for (const auto &node: frontier) {
                if (node.type != unresolved_type_id) {
                    grouped[physical_vertex_collection_for(node)].push_back(node);
                    continue;
                }
                unresolved.push_back(node);
            }

            if (!unresolved.empty()) {
                // ID routing MSBs identify ArangoDB, but not the document
                // collection. Resolve only the initial type-free frontier by
                // probing the materialized schema collections. Each probe uses
                // the per-collection primary _key index; no ID-to-type
                // directory is retained in application RAM.
                const auto materialized = available_node_collections();
                for (const auto &[type_id, node_type]: db_context_.schema->node_types()) {
                    (void) type_id;
                    if (!materialized.contains(node_type.name)) {
                        continue;
                    }
                    auto &group = grouped[node_type.name];
                    group.insert(group.end(), unresolved.begin(), unresolved.end());
                }
            }

            return grouped;
        }

        [[nodiscard]] std::string physical_vertex_collection_for(const NodeRef &node) const {
            // Frontier vertices are always sent to the home backend decoded from
            // their global ID. Therefore an Arango-routed frontier vertex is a real
            // document in its logical-type collection, never a cross-backend proxy.
            return db_context_.schema->node_type(node.type).name;
        }

        [[nodiscard]] std::unordered_set<std::string> available_node_collections() const {
            std::scoped_lock lock(collection_cache_mutex_);
            if (!node_collection_cache_.has_value()) {
                std::string auth;
                if (config_.credentials.has_value()) {
                    auth = config_.credentials->username + ":" + config_.credentials->password;
                }

                const auto collections = fetchAndSortCollections(collections_endpoint(), auth, collection_t::NODE);
                node_collection_cache_.emplace(collections.begin(), collections.end());
            }
            return *node_collection_cache_;
        }

        std::vector<TypeId> edge_types_for_query(const ExpandSpec &spec) const {
            const auto available_collections = available_edge_collections();
            std::vector<TypeId> result;

            const auto append_if_materialized = [&](const TypeId edge_type_id) {
                const auto &edge_type = db_context_.schema->edge_type(edge_type_id);
                if (available_collections.contains(edge_type.name)) {
                    result.push_back(edge_type_id);
                }
            };

            if (!spec.allowed_edge_types.empty()) {
                result.reserve(spec.allowed_edge_types.size());
                for (const TypeId edge_type_id: spec.allowed_edge_types) {
                    append_if_materialized(edge_type_id);
                }
                return result;
            }

            result.reserve(db_context_.schema->edge_types().size());
            for (const auto &[edge_type_id, edge_type]: db_context_.schema->edge_types()) {
                (void) edge_type;
                append_if_materialized(edge_type_id);
            }
            return result;
        }

        [[nodiscard]] std::unordered_set<std::string> available_edge_collections() const {
            std::unique_lock<std::mutex> lock;
            {
                EVENT_SCOPE_INFO(lock_span, "arango.collections.edge_cache_lock");
                lock = std::unique_lock<std::mutex>(collection_cache_mutex_);
                lock_span.add_end_field("cache_hit", edge_collection_cache_.has_value());
            }

            if (!edge_collection_cache_.has_value()) {
                std::string auth;
                if (config_.credentials.has_value()) {
                    auth = config_.credentials->username + ":" + config_.credentials->password;
                }

                EVENT_SCOPE_INFO(fetch_span, "arango.collections.edge_fetch", {"endpoint", collections_endpoint()});
                const auto collections = fetchAndSortCollections(collections_endpoint(), auth, collection_t::EDGE);
                edge_collection_cache_.emplace(collections.begin(), collections.end());
                fetch_span.add_end_field("collection_count", collections.size());
            }
            return *edge_collection_cache_;
        }

        [[nodiscard]] std::string collections_endpoint() const {
            const std::string marker = "/_api/";
            const std::size_t api_position = config_.endpoint.find(marker);
            if (api_position == std::string::npos) {
                throw std::runtime_error(
                        "ArangoDB endpoint must contain '/_api/' so the collection endpoint can be derived: " +
                        config_.endpoint);
            }
            return config_.endpoint.substr(0, api_position) + "/_api/collection";
        }

        static std::string arango_direction(Direction direction) {
            switch (direction) {
                case Direction::Directed:
                    return "OUTBOUND";

                case Direction::Undirected:
                case Direction::Both:
                    return "ANY";

                default:
                    throw std::invalid_argument("Unknown direction");
            }
        }

        static json node_to_write_row(const Node &node) {
            return json{{"key", global_id_to_string(node.global_id)},
                        {"uuid", global_id_to_signed_storage(node.global_id)},
                        {"type", node.type},
                        {"labels", node.labels},
                        {"properties", node.properties}};
        }

        static json edge_to_write_row(const Edge &edge) {
            return json{{"key", global_id_to_string(edge.global_id)},
                        {"uuid", global_id_to_signed_storage(edge.global_id)},
                        {"type", edge.type},
                        {"src_key", global_id_to_string(edge.src)},
                        {"dst_key", global_id_to_string(edge.dst)},
                        {"properties", edge.properties}};
        }

        void execute_write(const std::string &query, const json &bind_vars, const std::size_t expected_count) const {
            const json response = send_req(query, bind_vars);
            if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                throw std::runtime_error("ArangoDB insert query failed: " + response.dump());
            }
            if (!response.contains("result") || !response["result"].is_array() || response["result"].empty() ||
                !response["result"].front().is_number_integer()) {
                throw std::runtime_error("ArangoDB insert query returned no inserted count: " + response.dump());
            }
            const std::size_t inserted = response["result"].front().get<std::size_t>();
            if (inserted != expected_count) {
                throw std::runtime_error("ArangoDB inserted " + std::to_string(inserted) + " of " +
                                         std::to_string(expected_count) + " entities; an edge endpoint may be missing");
            }
        }

        [[nodiscard]] CurlHttpClient make_http_client() const {
            return CurlHttpClient(config_.credentials.has_value()
                                          ? config_.credentials->username + ":" + config_.credentials->password
                                          : "");
        }

        [[nodiscard]] json send_req(const std::string &query, const json &bind_vars) const {
            // Synchronous visitor/write paths may continue to reuse the
            // visitor-owned client. Async traversal paths call the overload
            // below with a request-local client.
            return send_req(http_, query, bind_vars, "", 0);
        }

        struct ParsedMinimalArangoPage {
            json metadata;
            std::size_t row_count{};
        };

        [[nodiscard]] ParsedMinimalArangoPage parse_minimal_arango_page_into_rows(
                const std::string &response_body, std::vector<MinimalAdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                        &seen_by_source,
                const ExpandSpec &spec) const {
            std::size_t row_count = 0;
            std::vector<bool> array_contains_consumed_row;

            const auto callback = [&](const int depth, const json::parse_event_t event, json &parsed) -> bool {
                const auto ensure_depth = [&array_contains_consumed_row](const int requested_depth) {
                    if (requested_depth < 0) {
                        return;
                    }

                    const auto required_size = static_cast<std::size_t>(requested_depth) + 1;
                    if (array_contains_consumed_row.size() < required_size) {
                        array_contains_consumed_row.resize(required_size, false);
                    }
                };

                if (event == json::parse_event_t::array_start) {
                    ensure_depth(depth);
                    array_contains_consumed_row[static_cast<std::size_t>(depth)] = false;
                    return true;
                }

                if (event != json::parse_event_t::array_end) {
                    return true;
                }

                ensure_depth(depth);

                const bool looks_like_row = is_minimal_result_row(parsed);

                if (looks_like_row) {
                    append_minimal_result_item(parsed, rows, row_by_source, seen_by_source, spec);
                    ++row_count;

                    if (depth > 0) {
                        ensure_depth(depth - 1);
                        array_contains_consumed_row[static_cast<std::size_t>(depth - 1)] = true;
                    }

                    // Do not retain the row inside the parsed response DOM.
                    return false;
                }

                // AQL UNION-style responses may wrap rows in additional arrays.
                // Propagate the discard upwards, but keep the top-level `result`
                // array so response validation still works.
                if (depth > 1 && array_contains_consumed_row[static_cast<std::size_t>(depth)]) {
                    ensure_depth(depth - 1);
                    array_contains_consumed_row[static_cast<std::size_t>(depth - 1)] = true;
                    return false;
                }

                return true;
            };

            json metadata = json::parse(response_body, callback);
            return ParsedMinimalArangoPage{.metadata = std::move(metadata), .row_count = row_count};
        }

        [[nodiscard]] std::size_t
        send_minimal_req_into_rows(CurlHttpClient &http, const std::string &query, const json &bind_vars,
                                   const std::string &vertex_collection, const std::size_t group_size,
                                   std::vector<MinimalAdjacencyRow> &rows,
                                   const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                                   const ExpandSpec &spec) const {
            std::string request_body;
            {
                EVENT_SCOPE_INFO(request_span, "arango.bfs_req_construct", {"vertex_collection", vertex_collection},
                                 {"bind_variable_count", bind_vars.size()});
                request_body = build_minimal_cursor_request_body(query, bind_vars);
                request_span.add_end_field("query_bytes", query.size());
                request_span.add_end_field("request_bytes", request_body.size());
            }
            return send_minimal_request_body_into_rows(http, request_body, vertex_collection, group_size, rows,
                                                       row_by_source, spec);
        }

        [[nodiscard]] static bool is_candidate_result_row(const json &item) {
            return item.is_array() && item.size() >= 2 && item[0].is_string() && item[1].is_string() &&
                   item[0].get_ref<const std::string &>().find('/') != std::string::npos;
        }

        [[nodiscard]] std::optional<NodeRef> minimal_result_neighbor_candidate(const json &item,
                                                                               const ExpandSpec &spec) const {
            if (!is_minimal_result_row(item) && !is_candidate_result_row(item)) {
                return std::nullopt;
            }

            GlobalId neighbor_id{};
            std::string logical_type_name;
            if (is_candidate_result_row(item) && !is_minimal_result_row(item)) {
                neighbor_id = global_id_from_document_id(item[0].get<std::string>());
                logical_type_name = item[1].get<std::string>();
            } else {
                const bool compact_edge_index_row =
                        item.size() >= 3 && item[0].is_string() && item[1].is_string() && item[2].is_string() &&
                        item[0].get_ref<const std::string &>().find('/') != std::string::npos &&
                        item[1].get_ref<const std::string &>().find('/') != std::string::npos;
                if (compact_edge_index_row) {
                    neighbor_id = global_id_from_document_id(item[1].get<std::string>());
                    logical_type_name = item[2].get<std::string>();
                } else {
                    neighbor_id = json_global_id(item[1]);
                    const std::string neighbor_document_id = item[2].get<std::string>();
                    logical_type_name = collection_from_arango_id(neighbor_document_id);
                    if (logical_type_name == "Proxy") {
                        if (item.size() < 4 || item[3].is_null() || !item[3].is_string() ||
                            item[3].get<std::string>().empty()) {
                            throw std::runtime_error("ArangoDB proxy neighbor is missing remote-type: " + item.dump());
                        }
                        logical_type_name = item[3].get<std::string>();
                    }
                }
            }

            const TypeId neighbor_type = db_context_.schema->node_type_id(logical_type_name);
            if (!spec.allows_target_type(neighbor_type)) {
                return std::nullopt;
            }
            return NodeRef{.global_id = neighbor_id, .type = neighbor_type};
        }

        [[nodiscard]] ParsedMinimalArangoPage
        parse_minimal_arango_page_to_consumer(const std::string &response_body, const ExpandSpec &spec,
                                              std::size_t candidate_batch_size,
                                              const NeighborBatchConsumer &consumer) const {
            std::size_t row_count = 0;
            std::vector<bool> array_contains_consumed_row;
            std::vector<NodeRef> candidates;
            candidates.reserve(candidate_batch_size);

            const auto flush = [&]() {
                if (!candidates.empty()) {
                    consumer(std::span<const NodeRef>(candidates));
                    candidates.clear();
                }
            };

            const auto callback = [&](const int depth, const json::parse_event_t event, json &parsed) -> bool {
                const auto ensure_depth = [&array_contains_consumed_row](const int requested_depth) {
                    if (requested_depth < 0) {
                        return;
                    }
                    const auto required_size = static_cast<std::size_t>(requested_depth) + 1;
                    if (array_contains_consumed_row.size() < required_size) {
                        array_contains_consumed_row.resize(required_size, false);
                    }
                };

                if (event == json::parse_event_t::array_start) {
                    ensure_depth(depth);
                    array_contains_consumed_row[static_cast<std::size_t>(depth)] = false;
                    return true;
                }
                if (event != json::parse_event_t::array_end) {
                    return true;
                }

                ensure_depth(depth);
                if (is_minimal_result_row(parsed) || is_candidate_result_row(parsed)) {
                    ++row_count;
                    if (auto candidate = minimal_result_neighbor_candidate(parsed, spec)) {
                        candidates.push_back(*candidate);
                        if (candidates.size() >= candidate_batch_size) {
                            flush();
                        }
                    }
                    if (depth > 0) {
                        ensure_depth(depth - 1);
                        array_contains_consumed_row[static_cast<std::size_t>(depth - 1)] = true;
                    }
                    return false;
                }

                if (depth > 1 && array_contains_consumed_row[static_cast<std::size_t>(depth)]) {
                    ensure_depth(depth - 1);
                    array_contains_consumed_row[static_cast<std::size_t>(depth - 1)] = true;
                    return false;
                }
                return true;
            };

            json metadata = json::parse(response_body, callback);
            flush();
            return ParsedMinimalArangoPage{.metadata = std::move(metadata), .row_count = row_count};
        }

        [[nodiscard]] std::size_t send_minimal_request_body_to_consumer(CurlHttpClient &http,
                                                                        const std::string &request_body,
                                                                        const std::string &vertex_collection,
                                                                        std::size_t group_size, const ExpandSpec &spec,
                                                                        std::size_t candidate_batch_size,
                                                                        const NeighborBatchConsumer &consumer) const {
            EVENT_SCOPE_INFO(total_span, "arango.bfs_http_query.streaming_candidates",
                             {"vertex_collection", vertex_collection}, {"group_size", group_size},
                             {"request_bytes", request_body.size()});

            std::string response_body;
            {
                EVENT_SCOPE_INFO(http_span, "arango.bfs_http_post", {"vertex_collection", vertex_collection},
                                 {"request_bytes", request_body.size()});
                response_body = http.post(config_.endpoint, request_body);
                http_span.add_end_field("response_bytes", response_body.size());
            }

            ParsedMinimalArangoPage page;
            {
                EVENT_SCOPE_INFO(parse_span, "arango.bfs_response_to_candidates_parse",
                                 {"vertex_collection", vertex_collection}, {"response_bytes", response_body.size()});
                page = parse_minimal_arango_page_to_consumer(response_body, spec, candidate_batch_size, consumer);
                parse_span.add_end_field("response_row_count", page.row_count);
                parse_span.add_end_field("has_more", page.metadata.value("hasMore", false));
            }

            if (page.metadata.value("error", false)) {
                throw std::runtime_error("ArangoDB minimal neighbor query failed: " + page.metadata.dump());
            }
            if (!page.metadata.contains("result") || !page.metadata["result"].is_array()) {
                throw std::runtime_error("ArangoDB minimal neighbor response has no result array: " +
                                         page.metadata.dump());
            }

            std::size_t total_row_count = page.row_count;
            std::size_t cursor_page_count = 1;
            std::size_t cursor_response_bytes = 0;
            while (page.metadata.value("hasMore", false)) {
                if (!page.metadata.contains("id") || !page.metadata["id"].is_string()) {
                    throw std::runtime_error("ArangoDB cursor says hasMore=true but response has no cursor id: " +
                                             page.metadata.dump());
                }
                const std::string next_url = config_.endpoint + "/" + page.metadata["id"].get<std::string>();
                const std::string next_response_body = http.put(next_url, "{}");
                cursor_response_bytes += next_response_body.size();
                page = parse_minimal_arango_page_to_consumer(next_response_body, spec, candidate_batch_size, consumer);
                ++cursor_page_count;
                total_row_count += page.row_count;

                if (page.metadata.value("error", false)) {
                    throw std::runtime_error("ArangoDB minimal neighbor cursor query failed: " + page.metadata.dump());
                }
                if (!page.metadata.contains("result") || !page.metadata["result"].is_array()) {
                    throw std::runtime_error("ArangoDB minimal neighbor cursor page has no result array: " +
                                             page.metadata.dump());
                }
            }

            total_span.add_end_field("cursor_page_count", cursor_page_count);
            total_span.add_end_field("cursor_response_bytes", cursor_response_bytes);
            total_span.add_end_field("response_row_count", total_row_count);
            total_span.add_end_field("status", "ok");
            return total_row_count;
        }

        [[nodiscard]] std::size_t send_minimal_request_body_into_rows(
                CurlHttpClient &http, const std::string &request_body, const std::string &vertex_collection,
                const std::size_t group_size, std::vector<MinimalAdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                const ExpandSpec &spec) const {
            EVENT_SCOPE_INFO(total_span, "arango.bfs_http_query", {"vertex_collection", vertex_collection},
                             {"group_size", group_size}, {"streaming_minimal_rows", true},
                             {"request_pre_serialized", true});

            std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                    seen_by_source;

            std::string response_body;
            {
                EVENT_SCOPE_INFO(http_span, "arango.bfs_http_post", {"vertex_collection", vertex_collection},
                                 {"request_bytes", request_body.size()});
                response_body = http.post(config_.endpoint, request_body);
                http_span.add_end_field("response_bytes", response_body.size());
            }

            ParsedMinimalArangoPage page;
            {
                EVENT_SCOPE_INFO(parse_span, "arango.bfs_response_to_minimal_rows_parse",
                                 {"vertex_collection", vertex_collection}, {"response_bytes", response_body.size()});
                page = parse_minimal_arango_page_into_rows(response_body, rows, row_by_source, seen_by_source, spec);
                parse_span.add_end_field("response_row_count", page.row_count);
                parse_span.add_end_field("has_more", page.metadata.value("hasMore", false));
            }

            if (page.metadata.value("error", false)) {
                throw std::runtime_error("ArangoDB minimal neighbor query failed: " + page.metadata.dump());
            }

            if (!page.metadata.contains("result") || !page.metadata["result"].is_array()) {
                throw std::runtime_error("ArangoDB minimal neighbor response has no result array: " +
                                         page.metadata.dump());
            }

            std::size_t total_row_count = page.row_count;
            std::size_t cursor_page_count = 1;
            std::size_t cursor_response_bytes = 0;

            if (page.metadata.value("hasMore", false)) {
                EVENT_SCOPE_INFO(cursor_span, "arango.bfs_cursor_drain", {"vertex_collection", vertex_collection},
                                 {"streaming_minimal_rows", true});

                while (page.metadata.value("hasMore", false)) {
                    if (!page.metadata.contains("id") || !page.metadata["id"].is_string()) {
                        throw std::runtime_error("ArangoDB cursor says hasMore=true but response has no cursor id: " +
                                                 page.metadata.dump());
                    }

                    const std::string next_url = config_.endpoint + "/" + page.metadata["id"].get<std::string>();
                    const std::string next_response_body = http.put(next_url, "{}");
                    cursor_response_bytes += next_response_body.size();

                    page = parse_minimal_arango_page_into_rows(next_response_body, rows, row_by_source, seen_by_source,
                                                               spec);
                    ++cursor_page_count;
                    total_row_count += page.row_count;

                    if (page.metadata.value("error", false)) {
                        throw std::runtime_error("ArangoDB minimal neighbor cursor query failed: " +
                                                 page.metadata.dump());
                    }

                    if (!page.metadata.contains("result") || !page.metadata["result"].is_array()) {
                        throw std::runtime_error("ArangoDB minimal neighbor cursor page has no result array: " +
                                                 page.metadata.dump());
                    }
                }

                cursor_span.add_end_field("cursor_page_count", cursor_page_count);
                cursor_span.add_end_field("cursor_response_bytes", cursor_response_bytes);
                cursor_span.add_end_field("cursor_result_count", total_row_count);
                cursor_span.add_end_field("status", "ok");
            }

            total_span.add_end_field("cursor_page_count", cursor_page_count);
            total_span.add_end_field("response_row_count", total_row_count);
            total_span.add_end_field("status", "ok");
            return total_row_count;
        }

        [[nodiscard]] json send_req(CurlHttpClient &http, const std::string &query, const json &bind_vars,
                                    const std::string &vertex_collection, const std::size_t group_size) const {
            EVENT_SCOPE_INFO(total_span, "arango.bfs_http_query", {"vertex_collection", vertex_collection},
                             {"group_size", group_size});

            json request;
            {
                EVENT_SCOPE_INFO(request_span, "arango.bfs_req_construct", {"vertex_collection", vertex_collection},
                                 {"bind_variable_count", bind_vars.size()});
                request = json{{"query", query},
                               {"bindVars", bind_vars},
                               // Large enough to avoid excessive cursor round-trips,
                               // but bounded so concurrent requests do not each ask
                               // ArangoDB for a million rows at once.
                               {"batchSize", 50000}};
                request_span.add_end_field("query_bytes", query.size());
            }

            std::string request_body;
            {
                EVENT_SCOPE_INFO(dump_span, "arango.bfs_json_req_dump", {"vertex_collection", vertex_collection});
                request_body = request.dump();
                dump_span.add_end_field("request_bytes", request_body.size());
            }

            std::string response_body;
            {
                EVENT_SCOPE_INFO(http_span, "arango.bfs_http_post", {"vertex_collection", vertex_collection},
                                 {"request_bytes", request_body.size()});
                response_body = http.post(config_.endpoint, request_body);
                http_span.add_end_field("response_bytes", response_body.size());
            }

            json response;
            {
                EVENT_SCOPE_INFO(parse_span, "arango.bfs_response_to_json_parse",
                                 {"vertex_collection", vertex_collection}, {"response_bytes", response_body.size()});
                response = json::parse(response_body);
                parse_span.add_end_field("result_count", response.contains("result") && response["result"].is_array()
                                                                 ? response["result"].size()
                                                                 : 0);
                parse_span.add_end_field("has_more", response.value("hasMore", false));
            }

            if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                total_span.add_end_field("status", "arango_error");
                return response;
            }

            if (!response.contains("result") || !response["result"].is_array()) {
                total_span.add_end_field("status", "missing_result");
                return response;
            }

            json combined_result = std::move(response["result"]);
            std::size_t cursor_page_count = 1;
            std::size_t cursor_response_bytes = 0;
            std::size_t cursor_result_count = combined_result.size();

            if (response.value("hasMore", false)) {
                // Keep pagination as one Arango-specific aggregate span rather
                // than one tiny span per cursor HTTP/JSON operation.
                EVENT_SCOPE_INFO(cursor_span, "arango.bfs_cursor_drain", {"vertex_collection", vertex_collection});

                while (response.value("hasMore", false)) {
                    if (!response.contains("id") || !response["id"].is_string()) {
                        throw std::runtime_error("ArangoDB cursor says hasMore=true but response has no cursor id: " +
                                                 response.dump());
                    }

                    const std::string next_url = config_.endpoint + "/" + response["id"].get<std::string>();
                    std::string next_response_body = http.put(next_url, "{}");
                    cursor_response_bytes += next_response_body.size();

                    response = json::parse(next_response_body);
                    ++cursor_page_count;

                    if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                        cursor_span.add_end_field("status", "arango_error");
                        return response;
                    }

                    if (!response.contains("result") || !response["result"].is_array()) {
                        throw std::runtime_error("ArangoDB cursor page does not contain result array: " +
                                                 response.dump());
                    }

                    cursor_result_count += response["result"].size();
                    for (auto &item: response["result"]) {
                        combined_result.push_back(std::move(item));
                    }
                }

                cursor_span.add_end_field("cursor_page_count", cursor_page_count);
                cursor_span.add_end_field("cursor_response_bytes", cursor_response_bytes);
                cursor_span.add_end_field("cursor_result_count", cursor_result_count);
                cursor_span.add_end_field("status", "ok");
            }

            response["result"] = std::move(combined_result);
            response["hasMore"] = false;

            total_span.add_end_field("cursor_page_count", cursor_page_count);
            total_span.add_end_field("combined_result_count", response["result"].size());
            total_span.add_end_field("status", "ok");
            return response;
        }

        static std::vector<std::string> fetchAndSortCollections(const std::string &url, const std::string &auth,
                                                                collection_t collection_type) {
            std::string raw_response = http_get(url, auth);
            std::vector<std::string> collections;

            auto data = json::parse(raw_response);

            for (const auto &col: data["result"]) {
                bool isSystem = col.value("isSystem", false);

                collection_t type =
                        static_cast<collection_t>(col.value("type", static_cast<std::size_t>(collection_t::NODE)));

                std::string name = col.value("name", "");

                if (isSystem || name.empty() || name[0] == '_') {
                    continue;
                }

                if (type == collection_type) {
                    collections.push_back(name);
                }
            }

            return collections;
        }

        static std::string get_union_query_for_collections(const std::vector<std::string> &collectionNames) {
            if (collectionNames.empty()) {
                return "RETURN []";
            }

            std::stringstream union_query;

            union_query << "RETURN UNION(";

            for (std::size_t i = 0; i < collectionNames.size(); ++i) {
                union_query << "(FOR d IN " << collectionNames[i] << " RETURN d)";

                if (i + 1 < collectionNames.size()) {
                    union_query << ", ";
                }
            }

            union_query << ")";

            return union_query.str();
        }

        std::string construct_all_query(collection_t collection_type) const {
            std::string auth;

            if (config_.credentials.has_value()) {
                auth = config_.credentials->username + ":" + config_.credentials->password;
            }

            /*
             * If your config endpoint is:
             *   http://localhost:8529/_db/university/_api/cursor
             *
             * Then collection endpoint should usually be:
             *   http://localhost:8529/_db/university/_api/collection
             *
             * For now, simplest option: keep it configured separately later.
             */
            auto collectionNames = fetchAndSortCollections(collections_endpoint(), auth, collection_type);

            return get_union_query_for_collections(collectionNames);
        }

        query_result_t evaluate(const std::string &query, const json &bindVars, ResultParseType parse_type) const {
            json response = send_req(query, bindVars);

            if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                throw std::runtime_error("ArangoDB returned error: " + response.dump());
            }

            return construct_result(response, parse_type);
        }

        query_result_t construct_result(const json &response, ResultParseType parse_type) const {
            auto result = std::make_unique<QueryResult>();

            if (!response.contains("result")) {
                throw std::runtime_error("ArangoDB response does not contain result: " + response.dump());
            }

            for (const auto &item: response["result"]) {
                /*
                 * construct_all_query returns one item which is an array.
                 * Flatten it.
                 */
                if (item.is_array()) {
                    for (const auto &nested: item) {
                        append_record(*result, nested, parse_type);
                    }
                } else {
                    append_record(*result, item, parse_type);
                }
            }

            return result;
        }

        void append_record(QueryResult &result, const json &item, ResultParseType parse_type) const {
            auto record = std::make_unique<Record>();

            switch (parse_type) {
                case ResultParseType::Node: {
                    record->fields.emplace_back(json_to_node(item));
                    break;
                }

                case ResultParseType::Relationship: {
                    record->fields.emplace_back(json_to_edge(item));
                    break;
                }

                case ResultParseType::AdjacencyRow: {
                    /*
                     * Expected item:
                     *
                     * {
                     *   "source": {...},
                     *   "neighbor": {...},
                     *   "edge": {...}
                     * }
                     */
                    if (!item.contains("source") || !item.contains("neighbor") || !item.contains("edge")) {
                        throw std::runtime_error("Arango adjacency item must contain source, neighbor, edge: " +
                                                 item.dump());
                    }

                    record->fields.emplace_back(json_to_node(item["source"]));
                    record->fields.emplace_back(json_to_node(item["neighbor"]));
                    record->fields.emplace_back(json_to_edge(item["edge"]));
                    break;
                }

                default:
                    throw std::invalid_argument("Unknown ResultParseType");
            }

            result.data.push_back(std::move(record));
        }

        Node json_to_node(const json &row) const {
            if (!row.contains("uuid")) {
                throw std::runtime_error("Arango node row does not contain uuid: " + row.dump());
            }

            const GlobalId global_id = json_global_id(row["uuid"]);

            /*
             * Important:
             *
             * For Arango, do NOT use row["type"] as the logical node type.
             * In your data, row["type"] may be a domain property, e.g. "program".
             *
             * Logical type comes from the collection name:
             *   _id = "Application/5185ab0e-..."
             *   logical type = "Application"
             */
            std::string logical_type_name = arango_collection_from_document(row);

            if (logical_type_name == "Proxy") {
                auto remote_type = get_string_property_opt(row, "remote-type");
                if (remote_type.has_value()) {
                    logical_type_name = *remote_type;
                }
            }

            return Node{.global_id = global_id,
                        .type = db_context_.schema->node_type_id(logical_type_name),
                        .labels = {logical_type_name},
                        .properties = arango_document_properties(row)};
        }

        static std::string arango_collection_from_document(const json &row) {
            if (row.contains("_id") && row["_id"].is_string()) {
                return collection_from_arango_id(row["_id"].get<std::string>());
            }

            /*
             * Fallback only for non-Arango-shaped projected documents.
             * Use this only if no _id exists.
             */
            if (row.contains("collection") && row["collection"].is_string()) {
                return row["collection"].get<std::string>();
            }

            throw std::runtime_error("Cannot infer Arango collection/logical type from document: " + row.dump());
        }

        static std::string collection_from_arango_id(const std::string &id) {
            const auto slash = id.find('/');

            if (slash == std::string::npos || slash == 0) {
                throw std::runtime_error("Invalid Arango _id: " + id);
            }

            return id.substr(0, slash);
        }

        static std::optional<std::string> get_string_property_opt(const json &row, const std::string &name) {
            if (row.contains(name) && row[name].is_string()) {
                return row[name].get<std::string>();
            }

            if (row.contains("properties") && row["properties"].is_object() && row["properties"].contains(name) &&
                row["properties"][name].is_string()) {
                return row["properties"][name].get<std::string>();
            }

            return std::nullopt;
        }

        static Map arango_document_properties(const json &row) {
            /*
             * If you ever return nested properties explicitly, support that too.
             */
            if (row.contains("properties") && row["properties"].is_object()) {
                return row["properties"].get<Map>();
            }

            /*
             * Otherwise your Arango document is flat:
             *
             * {
             *   "_id": "...",
             *   "_key": "...",
             *   "_rev": "...",
             *   "uuid": "...",
             *   "type": "program",
             *   "applicationId": "..."
             * }
             *
             * We keep user/domain fields such as:
             *   type = program
             *   applicationId = ...
             *   status = ...
             *
             * We remove Arango system fields.
             */
            json props = row;

            props.erase("_id");
            props.erase("_key");
            props.erase("_rev");

            /*
             * Optional:
             * If you do not want uuid duplicated in properties, erase it too.
             * If your current code expects properties["uuid"], leave it.
             */
            // props.erase("uuid");

            return props.get<Map>();
        }

        Edge json_to_edge(const json &row) const {
            if (!row.contains("uuid")) {
                throw std::runtime_error("Arango edge row does not contain uuid: " + row.dump());
            }

            const GlobalId global_id = json_global_id(row["uuid"]);

            /*
             * Same rule as nodes:
             * logical edge type comes from edge collection name, not row["type"].
             */
            const std::string logical_edge_type_name = arango_collection_from_document(row);

            GlobalId start_id{};
            GlobalId end_id{};

            if (row.contains("start") && row.contains("end")) {
                start_id = json_global_id(row["start"]);
                end_id = json_global_id(row["end"]);
            } else if (row.contains("_from") && row.contains("_to")) {
                start_id = global_id_from_string(uuid_from_arango_document_id(row["_from"].get<std::string>()));
                end_id = global_id_from_string(uuid_from_arango_document_id(row["_to"].get<std::string>()));
            } else {
                throw std::runtime_error("Arango edge row does not contain start/end or _from/_to: " + row.dump());
            }

            return Edge{.global_id = global_id,
                        .type = db_context_.schema->edge_type_id(logical_edge_type_name),
                        .src = start_id,
                        .dst = end_id,
                        .properties = arango_edge_properties(row)};
        }

        static Map arango_edge_properties(const json &row) {
            if (row.contains("properties") && row["properties"].is_object()) {
                return row["properties"].get<Map>();
            }

            json props = row;

            props.erase("_id");
            props.erase("_key");
            props.erase("_rev");
            props.erase("_from");
            props.erase("_to");

            /*
             * These are artificial fields added by your query:
             *
             * edge: MERGE(edge, {
             *   start: edgeStart.uuid,
             *   end: edgeEnd.uuid
             * })
             */
            props.erase("start");
            props.erase("end");

            // Optional:
            // props.erase("uuid");

            return props.get<Map>();
        }

        static std::string uuid_from_arango_document_id(const std::string &id) {
            /*
             * Arango _id shape:
             *
             *   CollectionName/key
             *
             * If your _key is the uuid, this works.
             */
            auto slash = id.find('/');

            if (slash == std::string::npos || slash + 1 >= id.size()) {
                throw std::runtime_error("Invalid Arango document id: " + id);
            }

            return id.substr(slash + 1);
        }
    };

    ArangoQueryVisitor::ArangoQueryVisitor(DatabaseContext &database_context, const BackendConnectionConfig &config,
                                           std::shared_ptr<BackendRequestScheduler> scheduler) :
        QueryVisitor(database_context), impl_(std::make_unique<Impl>(database_context, config, std::move(scheduler))) {}

    ArangoQueryVisitor::~ArangoQueryVisitor() = default;
    query_result_t ArangoQueryVisitor::visit(const GetAllVerticesQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const GetAllEdgesQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const GetNeighborsForVertexQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const GetVertexByIdQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const GetNeighborsBatchQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const InsertQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const InsertBatchQuery &query) { return impl_->visit(query); }

    std::vector<MinimalAdjacencyRow> ArangoQueryVisitor::get_minimal_neighbors_batch_fast(
            std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth, std::size_t batch_size,
            std::size_t max_concurrent_requests, ArangoRequestStrategy request_strategy,
            std::size_t single_request_max_frontier, std::size_t request_retries, std::size_t retry_delay_ms,
            const BackendRequestScheduling &scheduling) const {
        return impl_->get_minimal_neighbors_batch_fast(frontier, spec, depth, batch_size, max_concurrent_requests,
                                                       request_strategy, single_request_max_frontier, request_retries,
                                                       retry_delay_ms, scheduling);
    }

    void ArangoQueryVisitor::stream_neighbor_candidates_fast(
            std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth, std::size_t batch_size,
            std::size_t max_concurrent_requests, ArangoRequestStrategy request_strategy,
            std::size_t single_request_max_frontier, std::size_t request_retries, std::size_t retry_delay_ms,
            std::size_t candidate_batch_size, const NeighborBatchConsumer &consumer,
            const BackendRequestScheduling &scheduling) const {
        impl_->stream_neighbor_candidates_fast(frontier, spec, depth, batch_size, max_concurrent_requests,
                                               request_strategy, single_request_max_frontier, request_retries,
                                               retry_delay_ms, candidate_batch_size, consumer, scheduling);
    }

} // namespace polygraph
