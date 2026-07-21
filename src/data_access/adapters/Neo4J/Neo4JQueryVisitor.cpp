/**
 * @file src/data_access/adapters/Neo4J/Neo4JQueryVisitor.cpp
 * @brief Implementation for neo4 jquery visitor in the graph-store access module.
 */

#include "data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp"

#include "data_access/adapters/Neo4J/response_processors/LocallyDeployedNeo4JResponseProcessor.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "logging/level/Debug.hpp"
#include "logging/level/Info.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/Profiling.hpp"
#include "utilities/logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <data_access/adapters/Neo4J/Neo4JBoltClient.hpp>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <query/ResultParseType.hpp>
#include <query/query_types/GetAllEdgesQuery.hpp>
#include <query/query_types/GetAllVerticesQuery.hpp>
#include <query/query_types/GetNeighborsBatchQuery.hpp>
#include <query/query_types/GetNeighborsForVertexQuery.hpp>
#include <query/query_types/GetVertexByIdQuery.hpp>
#include <query/query_types/InsertBatchQuery.hpp>
#include <query/query_types/InsertQuery.hpp>
#include <serialization/RecordJsonSerializer.hpp>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utilities/curl.hpp>
#include <utilities/json.hpp>
#include <utilities/uuid.hpp>
#include <utility>
#include <vector>

namespace cpgdb {

    struct Neo4JQueryVisitor::Impl final : QueryVisitor {
        const BackendConnectionConfig &config_;
        LocallyDeployedNeo4JResponseProcessor response_processor_;
        bool use_bolt_ = true;
        std::string bolt_endpoint_;
        mutable std::mutex bolt_mutex_;
        mutable std::unique_ptr<Neo4JBoltClient> bolt_client_;

        const std::string getAllEdgesStrQuery = "MATCH ()-[r]->()\n"
                                                "RETURN {\n"
                                                "    native_id: id(r),\n"
                                                "    uuid: r.uuid,\n"
                                                "    type: type(r),\n"
                                                "    start_native_id: id(startNode(r)),\n"
                                                "    start_uuid: startNode(r).uuid,\n"
                                                "    start_is_proxy: coalesce(startNode(r).is_proxy, false),\n"
                                                "    end_native_id: id(endNode(r)),\n"
                                                "    end_uuid: endNode(r).uuid,\n"
                                                "    end_is_proxy: coalesce(endNode(r).is_proxy, false),\n"
                                                "    properties: properties(r)\n"
                                                "} AS relationship;";

        const std::string getAllVerticesStrQuery =
                "MATCH (n)\n"
                "RETURN {\n"
                "   native_id: id(n),\n"
                "   uuid: n.uuid,\n"
                "   is_proxy: coalesce(n.is_proxy, false),\n"
                "   type: coalesce(\n"
                "       CASE WHEN coalesce(n.is_proxy, false) THEN n.`remote-type` ELSE null END,\n"
                "       head([label IN labels(n) WHERE NOT label IN ['Proxy','CPGDBVertex','__GraphNode']]),\n"
                "       n.type\n"
                "   ),\n"
                "   labels: labels(n),\n"
                "   properties: properties(n)\n"
                "} AS node;";

        const std::string getVertexByIdStrQuery =
                "MATCH (n) WHERE id(n) = $nativeNodeId\n"
                "RETURN {\n"
                "   native_id: id(n), uuid: n.uuid, is_proxy: coalesce(n.is_proxy, false),\n"
                "   type: coalesce(head([label IN labels(n) WHERE NOT label IN "
                "['Proxy','CPGDBVertex','__GraphNode']]), n.type),\n"
                "   labels: labels(n), properties: properties(n)\n"
                "} AS node;";

        const std::string getNeighborsForVertexStrQuery =
                "MATCH (n) WHERE id(n) = $nativeNodeId\n"
                "MATCH (n)-[]-(neighbor)\n"
                "RETURN {\n"
                "   native_id: id(neighbor), uuid: neighbor.uuid, is_proxy: coalesce(neighbor.is_proxy, false),\n"
                "   type: coalesce(CASE WHEN coalesce(neighbor.is_proxy, false) THEN neighbor.`remote-type` ELSE null "
                "END,\n"
                "       head([label IN labels(neighbor) WHERE NOT label IN ['Proxy','CPGDBVertex','__GraphNode']]), "
                "neighbor.type),\n"
                "   labels: labels(neighbor), properties: properties(neighbor)\n"
                "} AS node;";


    public:
        Impl(DatabaseContext &db_context, const BackendConnectionConfig &config) :
            QueryVisitor(db_context), config_(config), response_processor_(db_context),
            use_bolt_(should_use_bolt(config)), bolt_endpoint_(resolve_bolt_endpoint(config)) {}

        query_result_t visit(const GetAllVerticesQuery &) override {
            return evaluate(getAllVerticesStrQuery, ResultParseType::Node, json::object());
        }

        query_result_t visit(const GetAllEdgesQuery &) override {
            return evaluate(getAllEdgesStrQuery, ResultParseType::Relationship, json::object());
        }

        query_result_t visit(const GetNeighborsForVertexQuery &q) override {
            return evaluate(getNeighborsForVertexStrQuery, ResultParseType::Node,
                            {{"nativeNodeId", neo4j_local_id(q.node().global_id)}});
        }

        query_result_t visit(const GetVertexByIdQuery &q) override {
            return evaluate(getVertexByIdStrQuery, ResultParseType::Node,
                            {{"nativeNodeId", neo4j_local_id(q.node().global_id)}});
        }

        query_result_t visit(const GetNeighborsBatchQuery &q) override {
            // CPGDB_PROFILE_FUNCTION();
            EVENT_SCOPE_INFO(_, "");

            json frontier = json::array();
            for (const auto &v: q.frontier()) {
                frontier.push_back(neo4j_local_id(v.global_id));
            }

            const auto edge_type_names = edge_type_names_for_spec(q.spec());
            const std::string edge_match =
                    build_edge_match(q.spec().direction, relationship_type_pattern(edge_type_names));

            const std::string query =
                    "UNWIND $frontier AS source_native_id "
                    "MATCH (source) WHERE id(source) = source_native_id " +
                    edge_match +
                    "RETURN "
                    "{native_id: id(source), uuid: source.uuid, is_proxy: coalesce(source.is_proxy,false), "
                    " type: coalesce(head([label IN labels(source) WHERE NOT label IN "
                    " ['Proxy','CPGDBVertex','__GraphNode']]), source.type), "
                    " labels: labels(source), properties: properties(source)} AS source, "
                    "{native_id: id(neighbor), uuid: neighbor.uuid, is_proxy: coalesce(neighbor.is_proxy,false), "
                    " type: coalesce(CASE WHEN coalesce(neighbor.is_proxy,false) THEN neighbor.`remote-type` END, "
                    " head([label IN labels(neighbor) WHERE NOT label IN "
                    " ['Proxy','CPGDBVertex','__GraphNode']]), neighbor.type), "
                    " labels: labels(neighbor), properties: properties(neighbor)} AS neighbor, "
                    "{native_id: id(r), uuid: r.uuid, type: type(r), "
                    " start_native_id: id(startNode(r)), start_uuid: startNode(r).uuid, "
                    " start_is_proxy: coalesce(startNode(r).is_proxy,false), "
                    " end_native_id: id(endNode(r)), end_uuid: endNode(r).uuid, "
                    " end_is_proxy: coalesce(endNode(r).is_proxy,false), properties: properties(r)} AS edge";

            return evaluate(query, ResultParseType::AdjacencyRow, {{"frontier", frontier}});
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

            json proxy_rows = json::array();
            std::unordered_map<TypeId, json> replica_edge_groups;

            for (const insertion::InsertionItem &item: query.items()) {
                if (item.is_node()) {
                    const Node &node = item.node();
                    if (home_backend(node.global_id) == NEO4J) {
                        throw std::runtime_error(
                                "Dynamic insertion of Neo4j-owned nodes is disabled in native-ID mode: "
                                "Neo4j must allocate id(n), after which the resolved graph IDs must be "
                                "propagated. Use tools/load_graph.py --reset for a fresh native-ID load.");
                    }
                    proxy_rows.push_back(proxy_node_to_write_row(node));
                    continue;
                }

                const Edge &edge = item.edge();
                if (home_backend(edge.global_id) == NEO4J) {
                    throw std::runtime_error(
                            "Dynamic insertion of Neo4j-owned relationships is disabled in native-ID "
                            "mode because Neo4j must allocate id(r). Use the bulk loader to regenerate "
                            "the resolved graph.");
                }
                json &rows = replica_edge_groups[edge.type];
                if (!rows.is_array()) {
                    rows = json::array();
                }
                rows.push_back(replica_edge_to_write_row(edge));
            }

            if (!proxy_rows.empty()) {
                const std::string cypher = "UNWIND $rows AS row "
                                           "MERGE (n:Proxy:CPGDBVertex:__GraphNode {uuid: row.uuid}) "
                                           "SET n = row.properties "
                                           "SET n.uuid = row.uuid, n.type = 'Proxy', n.is_proxy = true, "
                                           "    n.`remote-type` = row.remote_type";
                execute_write(cypher, {{"rows", proxy_rows}});
            }

            for (const auto &[edge_type, rows]: replica_edge_groups) {
                const std::string relationship =
                        escape_cypher_identifier(db_context_.schema->edge_type(edge_type).name);
                const std::string cypher = "UNWIND $rows AS row "
                                           "MATCH (source) WHERE "
                                           "  (row.src_proxy AND source:Proxy AND source.uuid = row.src) OR "
                                           "  (NOT row.src_proxy AND id(source) = row.src) "
                                           "MATCH (target) WHERE "
                                           "  (row.dst_proxy AND target:Proxy AND target.uuid = row.dst) OR "
                                           "  (NOT row.dst_proxy AND id(target) = row.dst) "
                                           "CREATE (source)-[r:" +
                                           relationship +
                                           "]->(target) "
                                           "SET r = row.properties "
                                           "SET r.uuid = row.uuid, r.type = row.type";
                execute_write(cypher, {{"rows", rows}});
            }

            return std::make_unique<QueryResult>();
        }

        std::vector<AdjacencyRow> get_neighbors_batch_fast(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                           int depth) const {

            return get_neighbors_batch_fast_async(frontier, spec, depth);
            // return get_neighbors_batch_fast_sequential(frontier, spec, depth);
        }

        std::vector<MinimalAdjacencyRow>
        get_minimal_neighbors_batch_fast(std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth,
                                         std::size_t batch_size, std::size_t max_concurrent_requests,
                                         std::size_t request_retries, std::size_t retry_delay_ms) const {
            /*
             * Keep all three calls together so benchmarking/switching requires
             * changing only these lines. The rich-row implementation is retained
             * as the third option.
             */
            // return get_minimal_neighbors_batch_direct_union(frontier, spec, depth);
            return get_minimal_neighbors_batch_direct_grouped_async(
                    frontier, spec, depth, batch_size, max_concurrent_requests, request_retries, retry_delay_ms);
            // return adjacency_rows_to_minimal(get_neighbors_batch_fast(frontier, spec, depth));
        }

    private:
        [[nodiscard]] GlobalId neo4j_local_id(const GlobalId id) const {
            return insertion::BackendEncodedId::local_id(id, db_context_.id_routing_backends);
        }

        [[nodiscard]] GlobalId neo4j_native_global_id(const json &value) const {
            if (!value.is_number_integer() && !value.is_number_unsigned()) {
                throw std::runtime_error("Neo4j native ID must be an integer: " + value.dump());
            }
            return insertion::BackendEncodedId::from_local_id(value.get<std::uint64_t>(), NEO4J,
                                                              db_context_.id_routing_backends);
        }

        static GlobalId stored_global_id(const json &value) {
            if (value.is_number_unsigned()) {
                return value.get<GlobalId>();
            }
            if (value.is_number_integer()) {
                return global_id_from_signed_storage(value.get<std::int64_t>());
            }
            if (value.is_string()) {
                return global_id_from_string(value.get<std::string>());
            }
            throw std::runtime_error("Stored global ID must be an integer or string: " + value.dump());
        }

        [[nodiscard]] GlobalId resolve_node_global_id(const json &native_id, const json &stored_id,
                                                      const bool is_proxy) const {
            if (is_proxy) {
                if (stored_id.is_null()) {
                    throw std::runtime_error("Neo4j proxy node is missing its numeric uuid property");
                }
                return stored_global_id(stored_id);
            }
            return neo4j_native_global_id(native_id);
        }

        [[nodiscard]] GlobalId resolve_edge_global_id(const json &native_id, const json &stored_id) const {
            return stored_id.is_null() ? neo4j_native_global_id(native_id) : stored_global_id(stored_id);
        }

        std::vector<AdjacencyRow> get_neighbors_batch_fast_sequential(std::span<const NodeRef> frontier,
                                                                      const ExpandSpec &spec, int depth) const {
            if (frontier.empty()) {
                return {};
            }

            // EVENT_SCOPE_DEBUG(total_span, "neo4j.get_neighbors_batch_fast", {"depth", depth},
            //                {"frontier_size", frontier.size()});

            std::vector<AdjacencyRow> rows;
            rows.reserve(frontier.size());

            std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> row_by_source;
            row_by_source.reserve(frontier.size());

            for (const auto &source: frontier) {
                auto [it, inserted] = row_by_source.emplace(source.global_id, rows.size());

                if (inserted) {
                    rows.push_back(AdjacencyRow{.source = source, .neighbors = {}});
                }
            }

            const auto grouped_frontier = group_frontier_by_physical_source_label(frontier);

            EVENT_SCOPE_INFO(total_span, "label_group_count", grouped_frontier.size());

            const auto edge_type_names = edge_type_names_for_spec(spec);
            for (const auto &[source_label, group]: grouped_frontier) {
                if (group.empty()) {
                    continue;
                }

                EVENT_SCOPE_INFO(group_span, "neo4j.label_group_neighbor_query", {"depth", depth},
                                 {"source_label", source_label}, {"group_size", group.size()});

                json frontier_json = json::array();

                for (const auto &source: group) {
                    frontier_json.push_back(neo4j_local_id(source.global_id));
                }

                const std::string query = "UNWIND $frontier AS source_native_id "
                                          "MATCH (source:" +
                                          escape_cypher_identifier(source_label) +
                                          ") "
                                          "WHERE id(source) = source_native_id " +
                                          build_edge_match(spec.direction, relationship_type_pattern(edge_type_names)) +
                                          rich_neighbor_projection();

                const json parameters{{"frontier", frontier_json}};

                json response;

                {
                    EVENT_SCOPE_INFO(http_span, "neo4j.bfs_http_query", {"depth", depth},
                                     {"source_label", source_label}, {"group_size", group.size()});

                    response = send_req(query, parameters);
                }

                {
                    EVENT_SCOPE_INFO(parse_span, "neo4j.json_to_intermediate_conversion", {"depth", depth},
                                     {"source_label", source_label}, {"group_size", group.size()});

                    parse_fast_response_into_rows(response, rows, row_by_source, spec);

                    EVENT_SCOPE_INFO_FIELD(parse_span, "source_rows", rows.size());
                }
            }

            EVENT_SCOPE_INFO_FIELD(total_span, "result_source_rows", rows.size());

            return rows;
        }

        struct Neo4jGroupResponse {
            std::string source_label;
            std::size_t group_size{};
            nlohmann::json response;
        };

        std::vector<AdjacencyRow> get_neighbors_batch_fast_async(std::span<const NodeRef> frontier,
                                                                 const ExpandSpec &spec, int depth) const {
            (void) depth;
            if (frontier.empty()) {
                return {};
            }

            EVENT_SCOPE_INFO(total_span, "neo4j.get_neighbors_batch_fast", {"frontier_size", frontier.size()});

            std::vector<AdjacencyRow> rows;
            rows.reserve(frontier.size());

            std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> row_by_source;
            row_by_source.reserve(frontier.size());

            for (const auto &source: frontier) {
                auto [it, inserted] = row_by_source.emplace(source.global_id, rows.size());

                if (inserted) {
                    rows.push_back(AdjacencyRow{.source = source, .neighbors = {}});
                }
            }

            const auto grouped_frontier = group_frontier_by_physical_source_label(frontier);
            const auto edge_type_names = edge_type_names_for_spec(spec);

            total_span.add_end_field("label_group_count", grouped_frontier.size());

            std::vector<std::future<Neo4jGroupResponse>> futures;

            for (const auto &[source_label, group]: grouped_frontier) {
                if (group.empty()) {
                    continue;
                }

                json frontier_json = json::array();

                for (const auto &source: group) {
                    frontier_json.push_back(neo4j_local_id(source.global_id));
                }

                const std::string query = "UNWIND $frontier AS source_native_id "
                                          "MATCH (source:" +
                                          escape_cypher_identifier(source_label) +
                                          ") "
                                          "WHERE id(source) = source_native_id " +
                                          build_edge_match(spec.direction, relationship_type_pattern(edge_type_names)) +
                                          rich_neighbor_projection();

                const json parameters{{"frontier", frontier_json}};

                futures.push_back(std::async(
                        std::launch::async,
                        [this, source_label, group_size = group.size(), query, parameters]() -> Neo4jGroupResponse {
                            EVENT_SCOPE_INFO(span, "neo4j.parallel_label_group_http_query",
                                             {"source_label", source_label}, {"group_size", group_size});

                            json response = send_req(query, parameters);

                            return Neo4jGroupResponse{.source_label = source_label,
                                                      .group_size = group_size,
                                                      .response = std::move(response)};
                        }));
            }

            for (auto &future: futures) {
                Neo4jGroupResponse result = future.get();

                EVENT_SCOPE_INFO(parse_span, "neo4j.json_to_intermediate_conversion",
                                 {"source_label", result.source_label}, {"group_size", result.group_size});

                parse_fast_response_into_rows(result.response, rows, row_by_source, spec);
            }

            total_span.add_end_field("result_source_rows", rows.size());

            return rows;
        }

        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_direct_union(std::span<const NodeRef> frontier,
                                                                                  const ExpandSpec &spec,
                                                                                  int depth) const {
            (void) depth;

            if (frontier.empty()) {
                return {};
            }

            EVENT_SCOPE_INFO(total_span, "neo4j.minimal_neighbors.direct_union", {"frontier_size", frontier.size()});

            auto [rows, row_by_source] = initialize_minimal_rows(frontier);
            const auto grouped_frontier = group_frontier_by_physical_source_label(frontier);

            if (grouped_frontier.empty()) {
                return rows;
            }

            const auto edge_type_names = edge_type_names_for_spec(spec);
            const std::string edge_match = build_edge_match(spec.direction, relationship_type_pattern(edge_type_names));
            const auto target_type_names = target_type_names_for_spec(spec);

            json parameters = json::object();
            parameters["target_types"] = target_type_names;

            std::string query;
            std::size_t branch_index = 0;

            for (const auto &[source_label, group]: grouped_frontier) {
                if (group.empty()) {
                    continue;
                }

                if (!query.empty()) {
                    query += " UNION ALL ";
                }

                const std::string parameter_name = "frontier_" + std::to_string(branch_index++);
                json group_frontier = json::array();
                group_frontier.get_ref<json::array_t &>().reserve(group.size());

                for (const auto &source: group) {
                    group_frontier.push_back(neo4j_local_id(source.global_id));
                }

                parameters[parameter_name] = std::move(group_frontier);

                // NOLINTBEGIN(performance-inefficient-string-concatenation)
                query += "UNWIND $" + parameter_name +
                         " AS source_native_id "
                         "MATCH (source:" +
                         escape_cypher_identifier(source_label) +
                         ") "
                         "WHERE id(source) = source_native_id " +
                         edge_match;
                // NOLINTEND(performance-inefficient-string-concatenation)

                if (!target_type_names.empty()) {
                    query += "WITH source, neighbor "
                             "WHERE neighbor.`remote-type` IN $target_types "
                             "   OR neighbor.type IN $target_types "
                             "   OR any(label IN labels(neighbor) WHERE label IN $target_types) ";
                }

                query += minimal_neighbor_projection();
            }

            if (query.empty()) {
                return rows;
            }

            const json response = send_req(query, parameters);
            parse_minimal_response_into_rows(response, rows, row_by_source, spec);

            total_span.add_end_field("result_source_rows", rows.size());
            return rows;
        }

        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_direct_grouped_async(
                std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth, std::size_t batch_size,
                std::size_t max_concurrent_requests, std::size_t request_retries, std::size_t retry_delay_ms) const {
            (void) depth;

            if (frontier.empty()) {
                return {};
            }
            if (batch_size == 0) {
                throw std::invalid_argument("Neo4j neighbor batch size must be greater than zero");
            }
            if (max_concurrent_requests == 0) {
                throw std::invalid_argument("Neo4j max concurrent requests must be greater than zero");
            }

            EVENT_SCOPE_INFO(total_span, "neo4j.minimal_neighbors.direct_grouped_bounded",
                             {"frontier_size", frontier.size()}, {"batch_size", batch_size},
                             {"max_concurrent_requests", max_concurrent_requests},
                             {"request_retries", request_retries});

            auto [rows, row_by_source] = initialize_minimal_rows(frontier);
            const auto grouped_frontier = group_frontier_by_physical_source_label(frontier);
            const auto edge_type_names = edge_type_names_for_spec(spec);
            const std::string edge_match = build_edge_match(spec.direction, relationship_type_pattern(edge_type_names));
            const auto target_type_names = target_type_names_for_spec(spec);

            struct Neo4jPreparedBatch {
                std::string source_label;
                std::size_t batch_index{};
                std::size_t batch_count{};
                std::vector<NodeRef> nodes;
                std::string query;
                json parameters;
            };

            struct Neo4jWorkerStats {
                std::size_t completed_requests{};
                std::size_t response_row_count{};
            };

            std::vector<Neo4jPreparedBatch> prepared_batches;

            for (const auto &[source_label, group]: grouped_frontier) {
                if (group.empty()) {
                    continue;
                }

                std::string query = "UNWIND $frontier AS source_native_id "
                                    "MATCH (source:" +
                                    escape_cypher_identifier(source_label) +
                                    ") "
                                    "WHERE id(source) = source_native_id " +
                                    edge_match;

                if (!target_type_names.empty()) {
                    query += "WITH source, neighbor "
                             "WHERE neighbor.`remote-type` IN $target_types "
                             "   OR neighbor.type IN $target_types "
                             "   OR any(label IN labels(neighbor) WHERE label IN $target_types) ";
                }

                query += minimal_neighbor_projection();

                const std::size_t batch_count = 1 + ((group.size() - 1) / batch_size);
                prepared_batches.reserve(prepared_batches.size() + batch_count);

                for (std::size_t offset = 0, batch_index = 0; offset < group.size();
                     offset += batch_size, ++batch_index) {
                    const std::size_t current_batch_size = std::min(batch_size, group.size() - offset);
                    std::vector<NodeRef> batch_nodes(group.begin() + static_cast<std::ptrdiff_t>(offset),
                                                     group.begin() +
                                                             static_cast<std::ptrdiff_t>(offset + current_batch_size));

                    json batch_frontier = json::array();
                    auto &frontier_array = batch_frontier.get_ref<json::array_t &>();
                    frontier_array.reserve(current_batch_size);
                    for (const NodeRef &source: batch_nodes) {
                        frontier_array.emplace_back(neo4j_local_id(source.global_id));
                    }

                    prepared_batches.push_back(
                            Neo4jPreparedBatch{.source_label = source_label,
                                               .batch_index = batch_index,
                                               .batch_count = batch_count,
                                               .nodes = std::move(batch_nodes),
                                               .query = query,
                                               .parameters = json{{"frontier", std::move(batch_frontier)},
                                                                  {"target_types", target_type_names}}});
                }
            }

            const std::size_t worker_count = std::min(max_concurrent_requests, prepared_batches.size());
            std::atomic_size_t next_batch{0};
            std::mutex rows_mutex;
            std::vector<std::future<Neo4jWorkerStats>> workers;
            workers.reserve(worker_count);

            for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
                workers.push_back(std::async(
                        std::launch::async,
                        [this, &prepared_batches, &next_batch, &rows, &row_by_source, &rows_mutex, spec,
                         request_retries, retry_delay_ms, worker_index]() mutable -> Neo4jWorkerStats {
                            Neo4jWorkerStats stats;
                            std::unique_ptr<Neo4JBoltClient> bolt_client;

                            while (true) {
                                const std::size_t prepared_index = next_batch.fetch_add(1, std::memory_order_relaxed);
                                if (prepared_index >= prepared_batches.size()) {
                                    break;
                                }

                                auto prepared = std::move(prepared_batches[prepared_index]);
                                const std::size_t current_batch_size = prepared.nodes.size();
                                EVENT_SCOPE_INFO(request_span, "neo4j.minimal_neighbors.direct_group_query",
                                                 {"source_label", prepared.source_label},
                                                 {"group_size", current_batch_size},
                                                 {"batch_index", prepared.batch_index},
                                                 {"batch_count", prepared.batch_count}, {"worker_index", worker_index});

                                for (std::size_t attempt = 0;; ++attempt) {
                                    try {
                                        if (use_bolt_ && !bolt_client) {
                                            bolt_client = make_bolt_client();
                                        }
                                        auto [batch_rows, batch_row_by_source] =
                                                initialize_minimal_rows(prepared.nodes);
                                        const std::size_t response_row_count = send_minimal_req_into_rows(
                                                prepared.query, prepared.parameters, batch_rows, batch_row_by_source,
                                                spec, prepared.source_label, current_batch_size, bolt_client.get());

                                        {
                                            EVENT_SCOPE_INFO(response_to_rows,
                                                             "neo4j.minimal_neighbors.response-into-rows",
                                                             {"label", prepared.source_label},
                                                             {"group_size", current_batch_size},
                                                             {"batch_index", prepared.batch_index},
                                                             {"batch_count", prepared.batch_count},
                                                             {"response_row_count", response_row_count});
                                            std::scoped_lock lock(rows_mutex);
                                            merge_minimal_rows(std::move(batch_rows), rows, row_by_source);
                                            response_to_rows.add_end_field("result_source_rows", rows.size());
                                        }

                                        request_span.add_end_field("attempts", attempt + 1);
                                        request_span.add_end_field("response_row_count", response_row_count);
                                        request_span.add_end_field("status", "ok");
                                        ++stats.completed_requests;
                                        stats.response_row_count += response_row_count;
                                        break;
                                    } catch (const std::exception &error) {
                                        if (attempt >= request_retries) {
                                            request_span.add_end_field("attempts", attempt + 1);
                                            request_span.add_end_field("status", "failed");
                                            request_span.add_end_field("error", error.what());
                                            throw std::runtime_error(
                                                    "Neo4j neighbor batch " + std::to_string(prepared.batch_index + 1) +
                                                    "/" + std::to_string(prepared.batch_count) + " for label '" +
                                                    prepared.source_label + "' failed after " +
                                                    std::to_string(attempt + 1) + " attempt(s): " + error.what());
                                        }

                                        const std::size_t delay =
                                                retry_delay_ms * (std::size_t{1} << std::min(attempt, std::size_t{10}));
                                        EVENT_SCOPE_INFO(retry_span, "neo4j.minimal_neighbors.request_retry",
                                                         {"source_label", prepared.source_label},
                                                         {"batch_index", prepared.batch_index},
                                                         {"attempt", attempt + 1}, {"delay_ms", delay},
                                                         {"error", error.what()});
                                        if (use_bolt_) {
                                            bolt_client.reset();
                                        }
                                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                                    }
                                }
                            }

                            return stats;
                        }));
            }

            total_span.add_end_field("request_count", prepared_batches.size());
            total_span.add_end_field("worker_count", worker_count);

            std::size_t completed_requests = 0;
            std::size_t total_response_rows = 0;
            for (auto &worker: workers) {
                const Neo4jWorkerStats stats = worker.get();
                completed_requests += stats.completed_requests;
                total_response_rows += stats.response_row_count;
            }
            total_span.add_end_field("completed_request_count", completed_requests);

            total_span.add_end_field("response_row_count", total_response_rows);
            total_span.add_end_field("result_source_rows", rows.size());
            return rows;
        }

        static std::string rich_neighbor_projection() {
            return "RETURN "
                   "id(source), id(neighbor), neighbor.uuid, "
                   "coalesce(neighbor.is_proxy, false), neighbor.`remote-type`, "
                   "neighbor.type, labels(neighbor), "
                   "id(r), r.uuid, type(r), "
                   "id(startNode(r)), startNode(r).uuid, coalesce(startNode(r).is_proxy, false), "
                   "id(endNode(r)), endNode(r).uuid, coalesce(endNode(r).is_proxy, false)";
        }

        static std::string minimal_neighbor_projection() {
            return "RETURN "
                   "id(source) AS source_native_id, "
                   "id(neighbor) AS neighbor_native_id, "
                   "neighbor.uuid AS neighbor_global_id, "
                   "coalesce(neighbor.is_proxy, false) AS neighbor_is_proxy, "
                   "neighbor.`remote-type` AS neighbor_remote_type, "
                   "neighbor.type AS neighbor_type_property, "
                   "labels(neighbor) AS neighbor_labels";
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

        void append_minimal_response_row(
                const json &row, std::vector<MinimalAdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                        &seen_by_source,
                const ExpandSpec &spec) const {
            if (!row.is_array() || row.size() < 7 || row[0].is_null() || row[1].is_null()) {
                return;
            }

            const bool looks_like_proxy = bool_or_false(row[3]) ||
                                          non_empty_string_opt(row[5]).value_or("") == "Proxy" ||
                                          labels_contain(row[6], "Proxy");
            const auto source_id = neo4j_native_global_id(row[0]);
            const auto neighbor_id = resolve_node_global_id(row[1], row[2], looks_like_proxy);
            const std::string neighbor_type_name =
                    resolve_logical_neighbor_type(row[3], row[4], row[5], row[6], global_id_to_string(neighbor_id));
            const auto source_iterator = row_by_source.find(source_id);

            if (source_iterator == row_by_source.end()) {
                return;
            }

            const TypeId neighbor_type = db_context_.schema->node_type_id(neighbor_type_name);

            if (!spec.allows_target_type(neighbor_type) || !seen_by_source[source_id].insert(neighbor_id).second) {
                return;
            }

            rows[source_iterator->second].neighbors.push_back(NodeRef{.global_id = neighbor_id, .type = neighbor_type});
        }

        void parse_minimal_response_into_rows(
                const json &response, std::vector<MinimalAdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                const ExpandSpec &spec) const {
            if (response.contains("errors") && !response["errors"].empty()) {
                throw std::runtime_error("Neo4J minimal neighbor query failed: " + response["errors"].dump());
            }

            if (!response.contains("results") || response["results"].empty() ||
                !response["results"][0].contains("data")) {
                return;
            }

            std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                    seen_by_source;

            for (const auto &item: response["results"][0]["data"]) {
                if (!item.contains("row") || !item["row"].is_array()) {
                    continue;
                }
                append_minimal_response_row(item["row"], rows, row_by_source, seen_by_source, spec);
            }
        }

        static std::vector<MinimalAdjacencyRow> adjacency_rows_to_minimal(const std::vector<AdjacencyRow> &rich_rows) {
            std::vector<MinimalAdjacencyRow> result;
            result.reserve(rich_rows.size());

            for (auto &row: rich_rows) {
                MinimalAdjacencyRow minimal{.source = row.source, .neighbors = {}};
                minimal.neighbors.reserve(row.neighbors.size());

                for (const auto &neighbor: row.neighbors) {
                    minimal.neighbors.push_back(neighbor.vertex);
                }

                result.push_back(std::move(minimal));
            }

            return result;
        }

        static std::string escape_cypher_identifier(const std::string &name) {
            std::string escaped;
            escaped.reserve(name.size() + 2);

            escaped.push_back('`');

            for (char c: name) {
                if (c == '`') {
                    escaped += "``";
                } else {
                    escaped.push_back(c);
                }
            }

            escaped.push_back('`');
            return escaped;
        }

        std::vector<std::string> edge_type_names_for_spec(const ExpandSpec &spec) const {
            std::vector<std::string> names;

            if (spec.allowed_edge_types.empty()) {
                const auto &descriptor = db_context_.backend_registry->descriptor(NEO4J);

                names.reserve(descriptor.coverage.edge_types.size());

                for (TypeId edge_type_id: descriptor.coverage.edge_types) {
                    names.push_back(db_context_.schema->edge_type(edge_type_id).name);
                }
            } else {
                names.reserve(spec.allowed_edge_types.size());

                for (TypeId edge_type_id: spec.allowed_edge_types) {
                    names.push_back(db_context_.schema->edge_type(edge_type_id).name);
                }
            }

            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());

            return names;
        }

        std::vector<std::string> target_type_names_for_spec(const ExpandSpec &spec) const {
            std::vector<std::string> names;
            names.reserve(spec.allowed_target_node_types.size());

            for (TypeId type_id: spec.allowed_target_node_types) {
                names.push_back(db_context_.schema->node_type(type_id).name);
            }

            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return names;
        }

        static std::string relationship_type_pattern(const std::vector<std::string> &edge_type_names) {
            if (edge_type_names.empty()) {
                return "";
            }

            std::string result = ":";

            for (std::size_t i = 0; i < edge_type_names.size(); ++i) {
                if (i > 0) {
                    result += "|";
                }

                result += escape_cypher_identifier(edge_type_names[i]);
            }

            return result;
        }


        static std::string build_edge_match(Direction direction, const std::string &relationship_type_pattern) {
            switch (direction) {
                case Direction::Directed:
                    return "MATCH (source)-[r" + relationship_type_pattern + "]->(neighbor) ";

                case Direction::Undirected:
                case Direction::Both:
                    return "MATCH (source)-[r" + relationship_type_pattern + "]-(neighbor) ";
            }

            throw std::runtime_error("Unknown Direction in Neo4JQueryVisitor");
        }

        std::string physical_source_label_for_neo4j(const NodeRef &node) const {
            if (node.type == unresolved_type_id) {
                throw std::logic_error("Cannot choose a Neo4j source label for an unresolved node type");
            }
            return db_context_.schema->node_type(node.type).name;
        }

        std::unordered_map<std::string, std::vector<NodeRef>>
        group_frontier_by_physical_source_label(std::span<const NodeRef> frontier) const {
            std::unordered_map<std::string, std::vector<NodeRef>> grouped;
            std::vector<NodeRef> unresolved;

            for (const auto &source: frontier) {
                if (source.type != unresolved_type_id) {
                    grouped[physical_source_label_for_neo4j(source)].push_back(source);
                    continue;
                }
                unresolved.push_back(source);
            }

            if (!unresolved.empty()) {
                // A caller-provided ID contains the owning backend but not the
                // logical node type. Resolve the first expansion through the
                // backend's label indexes instead of a process-wide ID map.
                // Once neighbors are returned, their logical types are carried
                // by the backend response and subsequent layers use one label.
                for (const auto &[type_id, node_type]: db_context_.schema->node_types()) {
                    (void) type_id;
                    auto &group = grouped[node_type.name];
                    group.insert(group.end(), unresolved.begin(), unresolved.end());
                }
            }

            return grouped;
        }

        static std::optional<std::string> non_empty_string_opt(const json &value) {
            if (!value.is_string()) {
                return std::nullopt;
            }

            auto result = value.get<std::string>();

            if (result.empty()) {
                return std::nullopt;
            }

            return result;
        }

        static bool bool_or_false(const json &value) { return value.is_boolean() && value.get<bool>(); }

        static bool contains_nul_byte(const std::string &value) { return value.find('\0') != std::string::npos; }

        static std::string printable_string(const std::string &value) {
            std::ostringstream oss;

            for (unsigned char c: value) {
                if (c == '\0') {
                    oss << "\\0";
                } else {
                    oss << static_cast<char>(c);
                }
            }

            return oss.str();
        }

        static bool labels_contain(const json &labels_value, const std::string &wanted) {
            if (!labels_value.is_array()) {
                return false;
            }

            return std::ranges::any_of(labels_value, [&](const auto &label_value) {
                return label_value.is_string() && label_value.template get<std::string>() == wanted;
            });
        }

        static bool is_infrastructure_label(const std::string &label) {
            return label == "Proxy" || label == "CPGDBVertex" || label == "__GraphNode";
        }

        static std::optional<std::string> first_logical_label_opt(const json &labels_value) {
            if (!labels_value.is_array()) {
                return std::nullopt;
            }

            for (const auto &label_value: labels_value) {
                if (!label_value.is_string()) {
                    continue;
                }

                std::string label = label_value.get<std::string>();

                if (label.empty()) {
                    continue;
                }

                if (is_infrastructure_label(label)) {
                    continue;
                }

                return label;
            }

            return std::nullopt;
        }

        static std::string resolve_logical_neighbor_uuid(const std::string &physical_uuid,
                                                         const json &remote_uuid_value, bool looks_like_proxy) {
            if (looks_like_proxy) {
                auto remote_uuid = non_empty_string_opt(remote_uuid_value);

                if (remote_uuid.has_value()) {
                    return *remote_uuid;
                }
            }

            return physical_uuid;
        }

        static std::string resolve_logical_neighbor_type(const json &is_proxy_value, const json &remote_type_value,
                                                         const json &type_property_value, const json &labels_value,
                                                         const std::string &neighbor_uuid) {
            const bool is_proxy_flag = bool_or_false(is_proxy_value);

            const auto remote_type = non_empty_string_opt(remote_type_value);
            const auto type_property = non_empty_string_opt(type_property_value);
            const auto logical_label = first_logical_label_opt(labels_value);

            const bool looks_like_proxy = is_proxy_flag || labels_contain(labels_value, "Proxy") ||
                                          (type_property.has_value() && *type_property == "Proxy");

            if (looks_like_proxy) {
                if (!remote_type.has_value()) {
                    throw std::runtime_error(
                            "Neo4J proxy node is missing remote-type. neighbor_uuid=" + neighbor_uuid +
                            ", is_proxy=" + is_proxy_value.dump() + ", remote_type=" + remote_type_value.dump() +
                            ", type_property=" + type_property_value.dump() + ", labels=" + labels_value.dump());
                }

                return *remote_type;
            }

            if (logical_label.has_value()) {
                return *logical_label;
            }

            if (type_property.has_value() && *type_property != "Proxy") {
                return *type_property;
            }

            throw std::runtime_error(
                    "Neo4J fast path could not resolve logical type for neighbor uuid=" + neighbor_uuid +
                    ", remote_type=" + remote_type_value.dump() + ", type_property=" + type_property_value.dump() +
                    ", labels=" + labels_value.dump());
        }

        void parse_fast_response_into_rows(
                const json &response, std::vector<AdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                const ExpandSpec &spec) const {
            if (response.contains("errors") && !response["errors"].empty()) {
                throw std::runtime_error("Neo4J fast neighbor query failed: " + response["errors"].dump());
            }

            if (!response.contains("results") || response["results"].empty() ||
                !response["results"][0].contains("data")) {
                return;
            }

            const auto &data = response["results"][0]["data"];

            std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                    seen_by_source;

            for (const auto &item: data) {
                if (!item.contains("row") || !item["row"].is_array()) {
                    continue;
                }

                const auto &r = item["row"];

                if (r.size() < 16 || r[0].is_null() || r[1].is_null()) {
                    continue;
                }

                const bool looks_like_proxy = bool_or_false(r[3]) ||
                                              non_empty_string_opt(r[5]).value_or("") == "Proxy" ||
                                              labels_contain(r[6], "Proxy");
                const auto source_id = neo4j_native_global_id(r[0]);
                const auto neighbor_id = resolve_node_global_id(r[1], r[2], looks_like_proxy);
                const std::string neighbor_type_name =
                        resolve_logical_neighbor_type(r[3], r[4], r[5], r[6], global_id_to_string(neighbor_id));

                auto row_it = row_by_source.find(source_id);
                if (row_it == row_by_source.end() || !seen_by_source[source_id].insert(neighbor_id).second) {
                    continue;
                }

                const TypeId neighbor_type = db_context_.schema->node_type_id(neighbor_type_name);
                if (!spec.allows_target_type(neighbor_type)) {
                    continue;
                }

                const std::string edge_type_name = r[9].get<std::string>();
                const TypeId edge_type = db_context_.schema->edge_type_id(edge_type_name);
                if (!spec.allows_edge_type(edge_type)) {
                    continue;
                }

                const bool start_proxy = bool_or_false(r[12]);
                const bool end_proxy = bool_or_false(r[15]);
                EdgeRef edge_ref{.global_id = resolve_edge_global_id(r[7], r[8]),
                                 .type = edge_type,
                                 .from = resolve_node_global_id(r[10], r[11], start_proxy),
                                 .to = resolve_node_global_id(r[13], r[14], end_proxy)};

                rows[row_it->second].neighbors.push_back(NeighborRef{
                        .vertex = NodeRef{.global_id = neighbor_id, .type = neighbor_type}, .via_edge = edge_ref});
            }
        }

        [[nodiscard]] BackendId home_backend(const GlobalId id) const {
            return insertion::BackendEncodedId::decode(id, db_context_.id_routing_backends);
        }

        [[nodiscard]] json endpoint_storage_id(const GlobalId id) const {
            if (home_backend(id) == NEO4J) {
                return neo4j_local_id(id);
            }
            return global_id_to_signed_storage(id);
        }

        [[nodiscard]] json proxy_node_to_write_row(const Node &node) const {
            json properties = node.properties;
            properties.erase("uuid");
            return json{{"uuid", global_id_to_signed_storage(node.global_id)},
                        {"remote_type", db_context_.schema->node_type(node.type).name},
                        {"properties", std::move(properties)}};
        }

        [[nodiscard]] json replica_edge_to_write_row(const Edge &edge) const {
            json properties = edge.properties;
            properties.erase("uuid");
            return json{{"uuid", global_id_to_signed_storage(edge.global_id)},
                        {"type", edge.type},
                        {"src", endpoint_storage_id(edge.src)},
                        {"dst", endpoint_storage_id(edge.dst)},
                        {"src_proxy", home_backend(edge.src) != NEO4J},
                        {"dst_proxy", home_backend(edge.dst) != NEO4J},
                        {"properties", std::move(properties)}};
        }

        void execute_write(const std::string &query, const json &parameters) const {
            const json response = send_req(query, parameters);
            if (response.contains("errors") && response["errors"].is_array() && !response["errors"].empty()) {
                throw std::runtime_error("Neo4J insert query failed: " + response["errors"].dump());
            }
            if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                throw std::runtime_error("Neo4J insert query failed: " + response.dump());
            }
        }

        [[nodiscard]] std::size_t
        send_minimal_req_into_rows(const std::string &query, const json &parameters,
                                   std::vector<MinimalAdjacencyRow> &rows,
                                   const std::unordered_map<GlobalId, std::size_t, std::hash<GlobalId>> &row_by_source,
                                   const ExpandSpec &spec, const std::string &source_label,
                                   const std::size_t group_size, Neo4JBoltClient *bolt_client) const {
            std::unordered_map<GlobalId, std::unordered_set<GlobalId, std::hash<GlobalId>>, std::hash<GlobalId>>
                    seen_by_source;

            if (use_bolt_) {
                if (bolt_client == nullptr) {
                    throw std::logic_error("Neo4j async minimal request requires a request-local Bolt client");
                }

                EVENT_SCOPE_INFO(bolt_span, "neo4j.bfs_bolt_query", {"source_label", source_label},
                                 {"group_size", group_size}, {"streaming_minimal_rows", false});
                const json response = bolt_client->run(query, parameters);
                const std::size_t response_row_count = response.contains("results") && response["results"].is_array() &&
                                                                       !response["results"].empty() &&
                                                                       response["results"][0].contains("data") &&
                                                                       response["results"][0]["data"].is_array()
                                                               ? response["results"][0]["data"].size()
                                                               : 0;
                parse_minimal_response_into_rows(response, rows, row_by_source, spec);
                bolt_span.add_end_field("response_row_count", response_row_count);
                return response_row_count;
            }

            EVENT_SCOPE_INFO(total_span, "neo4j.bfs_http_query", {"source_label", source_label},
                             {"group_size", group_size}, {"streaming_minimal_rows", true});

            json request;
            {
                EVENT_SCOPE_INFO(request_span, "neo4j.bfs_req_construct", {"source_label", source_label},
                                 {"group_size", group_size});
                request = json{{"statements", json::array({{{"statement", query}, {"parameters", parameters}}})}};
                request_span.add_end_field("query_bytes", query.size());
            }

            std::string request_body;
            {
                EVENT_SCOPE_INFO(dump_span, "neo4j.bfs_json_req_dump", {"source_label", source_label});
                request_body = request.dump();
                dump_span.add_end_field("request_bytes", request_body.size());
            }

            std::string auth;
            if (config_.credentials.has_value()) {
                auth = config_.credentials->username + ":" + config_.credentials->password;
            }

            std::string response_body;
            {
                EVENT_SCOPE_INFO(http_span, "neo4j.bfs_http_post", {"source_label", source_label},
                                 {"request_bytes", request_body.size()});
                response_body = http_post(config_.endpoint, request_body, auth);
                http_span.add_end_field("response_bytes", response_body.size());
            }

            std::size_t response_row_count = 0;
            std::vector<bool> object_contains_consumed_row;

            const auto callback = [&](const int depth, const json::parse_event_t event, json &parsed) -> bool {
                const auto ensure_depth = [&object_contains_consumed_row](const int requested_depth) {
                    if (requested_depth < 0) {
                        return;
                    }

                    const auto required_size = static_cast<std::size_t>(requested_depth) + 1;
                    if (object_contains_consumed_row.size() < required_size) {
                        object_contains_consumed_row.resize(required_size, false);
                    }
                };

                if (event == json::parse_event_t::object_start) {
                    ensure_depth(depth);
                    object_contains_consumed_row[static_cast<std::size_t>(depth)] = false;
                    return true;
                }

                if (event == json::parse_event_t::array_end) {
                    const bool looks_like_minimal_row =
                            parsed.is_array() && parsed.size() >= 7 && parsed[0].is_number_integer() &&
                            parsed[1].is_number_integer() &&
                            (parsed[2].is_null() || parsed[2].is_number_integer() || parsed[2].is_string()) &&
                            (parsed[3].is_boolean() || parsed[3].is_null()) &&
                            (parsed[4].is_string() || parsed[4].is_null()) &&
                            (parsed[5].is_string() || parsed[5].is_null()) && parsed[6].is_array();

                    if (!looks_like_minimal_row) {
                        return true;
                    }

                    append_minimal_response_row(parsed, rows, row_by_source, seen_by_source, spec);
                    ++response_row_count;

                    if (depth > 0) {
                        ensure_depth(depth - 1);
                        object_contains_consumed_row[static_cast<std::size_t>(depth - 1)] = true;
                    }

                    // The row has already been converted to MinimalAdjacencyRow.
                    // Discard it from the JSON DOM immediately.
                    return false;
                }

                if (event == json::parse_event_t::object_end) {
                    ensure_depth(depth);
                    if (object_contains_consumed_row[static_cast<std::size_t>(depth)]) {
                        // Discard the Neo4j data wrapper object as well. This
                        // prevents one empty object per result row from being
                        // retained in results[0].data.
                        return false;
                    }
                }

                return true;
            };

            json metadata;
            {
                EVENT_SCOPE_INFO(parse_span, "neo4j.bfs_response_to_minimal_rows_parse", {"source_label", source_label},
                                 {"response_bytes", response_body.size()});
                metadata = json::parse(response_body, callback);
                parse_span.add_end_field("response_row_count", response_row_count);
            }

            if (metadata.contains("errors") && metadata["errors"].is_array() && !metadata["errors"].empty()) {
                throw std::runtime_error("Neo4J minimal neighbor query failed: " + metadata["errors"].dump());
            }

            if (!metadata.contains("results") || !metadata["results"].is_array() || metadata["results"].empty() ||
                !metadata["results"][0].contains("data")) {
                throw std::runtime_error("Neo4J minimal neighbor response has no results[0].data: " + metadata.dump());
            }

            total_span.add_end_field("response_row_count", response_row_count);
            total_span.add_end_field("status", "ok");
            return response_row_count;
        }

        json send_req(const std::string &query, const json &parameters) const {
            // CPGDB_PROFILE_FUNCTION();

            if (use_bolt_) {
                // CPGDB_PROFILE_SCOPE("Neo4J send_req: Bolt RUN/PULL");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_bolt_query");
                return send_req_bolt(query, parameters);
            }
            EVENT_SCOPE_INFO(_, "neo4j.bfs_http_query");

            json req;

            {
                // CPGDB_PROFILE_SCOPE("Neo4J send_req: build request JSON");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_req_construct");
                req = json{{"statements", json::array({{{"statement", query}, {"parameters", parameters}}})}};
            }

            std::string request_body;

            {
                // CPGDB_PROFILE_SCOPE("Neo4J send_req: dump request JSON");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_json_req_dump");
                request_body = req.dump();
            }

            std::string response_body;
            std::string auth;

            if (config_.credentials.has_value()) {
                auth = config_.credentials->username + ":" + config_.credentials->password;
            }

            {
                // CPGDB_PROFILE_SCOPE("Neo4J send_req: HTTP POST");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_http_post");
                response_body = http_post(config_.endpoint, request_body, auth);
            }

            json parsed;

            {
                // CPGDB_PROFILE_SCOPE("Neo4J send_req: parse response JSON");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_response_to_json_parse");
                parsed = json::parse(response_body);
            }

            EVENT_SCOPE_INFO(_a, "neo4j.debug");
            return parsed;
        }

        static bool should_use_bolt(const BackendConnectionConfig &config) {
            if (const char *env = std::getenv("CPGDB_NEO4J_TRANSPORT")) {
                return std::string(env) == "bolt";
            }

            return config.transport == "bolt";
        }

        [[nodiscard]] std::unique_ptr<Neo4JBoltClient> make_bolt_client() const {
            std::string username;
            std::string password;

            if (config_.credentials.has_value()) {
                username = config_.credentials->username;
                password = config_.credentials->password;
            }

            return std::make_unique<Neo4JBoltClient>(bolt_endpoint_, username, password, config_.database_name);
        }

        static std::string resolve_bolt_endpoint(const BackendConnectionConfig &config) {
            if (!config.bolt_endpoint.empty()) {
                return config.bolt_endpoint;
            }

            // Reasonable local default for docker-compose Neo4J.
            // Keep HTTP endpoint unchanged so users can switch transport via env var.
            return "bolt://localhost:7687";
        }

        json send_req_bolt(const std::string &query, const json &parameters) const {
            std::lock_guard<std::mutex> lock(bolt_mutex_);

            if (!bolt_client_) {
                std::string username;
                std::string password;

                if (config_.credentials.has_value()) {
                    username = config_.credentials->username;
                    password = config_.credentials->password;
                }

                bolt_client_ =
                        std::make_unique<Neo4JBoltClient>(bolt_endpoint_, username, password, config_.database_name);
            }

            return bolt_client_->run(query, parameters);
        }

        [[nodiscard]] query_result_t evaluate(const std::string &query, ResultParseType parse_type,
                                              const json &parameters) const {
            // CPGDB_PROFILE_FUNCTION();

            json response;

            {
                EVENT_SCOPE_INFO(_, "neo4j.http_bfs_query");
                response = send_req(query, parameters);
            }

            {
                EVENT_SCOPE_INFO(_, "neo4j.json_to_intermediate_conversion");
                return response_processor_.process(response, parse_type);
            }
        }
    };

    Neo4JQueryVisitor::Neo4JQueryVisitor(DatabaseContext &database_context, const BackendConnectionConfig &config) :
        QueryVisitor(database_context), impl_(std::make_unique<Impl>(database_context, config)) {}

    Neo4JQueryVisitor::~Neo4JQueryVisitor() = default;

    query_result_t Neo4JQueryVisitor::visit(const GetAllVerticesQuery &query) { return impl_->visit(query); }
    query_result_t Neo4JQueryVisitor::visit(const GetAllEdgesQuery &query) { return impl_->visit(query); }
    query_result_t Neo4JQueryVisitor::visit(const GetNeighborsForVertexQuery &query) { return impl_->visit(query); }
    query_result_t Neo4JQueryVisitor::visit(const GetVertexByIdQuery &query) { return impl_->visit(query); }
    query_result_t Neo4JQueryVisitor::visit(const GetNeighborsBatchQuery &query) { return impl_->visit(query); }

    query_result_t Neo4JQueryVisitor::visit(const InsertQuery &query) { return impl_->visit(query); }
    query_result_t Neo4JQueryVisitor::visit(const InsertBatchQuery &query) { return impl_->visit(query); }

    std::vector<AdjacencyRow> Neo4JQueryVisitor::get_neighbors_batch_fast(std::span<const NodeRef> frontier,
                                                                          const ExpandSpec &spec, int depth) const {
        return impl_->get_neighbors_batch_fast(frontier, spec, depth);
    }

    std::vector<MinimalAdjacencyRow> Neo4JQueryVisitor::get_minimal_neighbors_batch_fast(
            std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth, std::size_t batch_size,
            std::size_t max_concurrent_requests, std::size_t request_retries, std::size_t retry_delay_ms) const {
        return impl_->get_minimal_neighbors_batch_fast(frontier, spec, depth, batch_size, max_concurrent_requests,
                                                       request_retries, retry_delay_ms);
    }

} // namespace cpgdb
