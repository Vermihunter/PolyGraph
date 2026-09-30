/**
 * @file src/data_access/adapters/Neo4J/Neo4JQueryVisitor.cpp
 * @brief Implementation for neo4 jquery visitor in the graph-store access module.
 */

#include "data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp"

#include "data_access/adapters/Neo4J/response_processors/LocallyDeployedNeo4JResponseProcessor.hpp"
#include "data_model/AdjacencyRow.hpp"
#include "events/Events.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/Profiling.hpp"

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

namespace polygraph {

    struct Neo4JQueryVisitor::Impl final : QueryVisitor {
        const BackendConnectionConfig &config_;
        std::shared_ptr<BackendRequestScheduler> scheduler_;
        LocallyDeployedNeo4JResponseProcessor response_processor_;
        bool use_bolt_ = true;
        std::string bolt_endpoint_;
        mutable std::mutex bolt_mutex_;
        mutable std::unique_ptr<Neo4JBoltClient> bolt_client_;

        const std::string getAllEdgesStrQuery = "MATCH ()-[r]->()\n"
                                                "RETURN {\n"
                                                "    native_id: r.localId,\n"
                                                "    uuid: r.uuid,\n"
                                                "    type: type(r),\n"
                                                "    start_native_id: startNode(r).localId,\n"
                                                "    start_uuid: startNode(r).uuid,\n"
                                                "    start_is_proxy: coalesce(startNode(r).is_proxy, false),\n"
                                                "    end_native_id: endNode(r).localId,\n"
                                                "    end_uuid: endNode(r).uuid,\n"
                                                "    end_is_proxy: coalesce(endNode(r).is_proxy, false),\n"
                                                "    properties: properties(r)\n"
                                                "} AS relationship;";

        const std::string getAllVerticesStrQuery =
                "MATCH (n)\n"
                "RETURN {\n"
                "   native_id: n.localId,\n"
                "   uuid: n.uuid,\n"
                "   is_proxy: coalesce(n.is_proxy, false),\n"
                "   type: coalesce(\n"
                "       CASE WHEN coalesce(n.is_proxy, false) THEN n.`remote-type` ELSE null END,\n"
                "       head([label IN labels(n) WHERE NOT label IN ['Proxy','POLYGRAPHVertex','__GraphNode']]),\n"
                "       n.type\n"
                "   ),\n"
                "   labels: labels(n),\n"
                "   properties: properties(n)\n"
                "} AS node;";

        const std::string getVertexByIdStrQuery =
                "MATCH (n:POLYGRAPHVertex {localId: $nativeNodeId})\n"
                "RETURN {\n"
                "   native_id: n.localId, uuid: n.uuid, is_proxy: coalesce(n.is_proxy, false),\n"
                "   type: coalesce(head([label IN labels(n) WHERE NOT label IN "
                "['Proxy','POLYGRAPHVertex','__GraphNode']]), n.type),\n"
                "   labels: labels(n), properties: properties(n)\n"
                "} AS node;";

        const std::string getNeighborsForVertexStrQuery =
                "MATCH (n:POLYGRAPHVertex {localId: $nativeNodeId})\n"
                "MATCH (n)-[]-(neighbor)\n"
                "RETURN {\n"
                "   native_id: neighbor.localId, uuid: neighbor.uuid, is_proxy: coalesce(neighbor.is_proxy, false),\n"
                "   type: coalesce(CASE WHEN coalesce(neighbor.is_proxy, false) THEN neighbor.`remote-type` ELSE null "
                "END,\n"
                "       head([label IN labels(neighbor) WHERE NOT label IN "
                "['Proxy','POLYGRAPHVertex','__GraphNode']]), "
                "neighbor.type),\n"
                "   labels: labels(neighbor), properties: properties(neighbor)\n"
                "} AS node;";


    public:
        Impl(DatabaseContext &db_context, const BackendConnectionConfig &config,
             std::shared_ptr<BackendRequestScheduler> scheduler) :
            QueryVisitor(db_context), config_(config), scheduler_(std::move(scheduler)),
            response_processor_(db_context), use_bolt_(should_use_bolt(config)),
            bolt_endpoint_(resolve_bolt_endpoint(config)) {
            if (!scheduler_) {
                throw std::invalid_argument("Neo4JQueryVisitor requires a backend request scheduler");
            }
        }

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
            // POLYGRAPH_PROFILE_FUNCTION();
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
                    "MATCH (source:POLYGRAPHVertex {localId: source_native_id}) " +
                    edge_match +
                    "RETURN "
                    "{native_id: source.localId, uuid: source.uuid, is_proxy: coalesce(source.is_proxy,false), "
                    " type: coalesce(head([label IN labels(source) WHERE NOT label IN "
                    " ['Proxy','POLYGRAPHVertex','__GraphNode']]), source.type), "
                    " labels: labels(source), properties: properties(source)} AS source, "
                    "{native_id: neighbor.localId, uuid: neighbor.uuid, is_proxy: coalesce(neighbor.is_proxy,false), "
                    " type: coalesce(CASE WHEN coalesce(neighbor.is_proxy,false) THEN neighbor.`remote-type` END, "
                    " head([label IN labels(neighbor) WHERE NOT label IN "
                    " ['Proxy','POLYGRAPHVertex','__GraphNode']]), neighbor.type), "
                    " labels: labels(neighbor), properties: properties(neighbor)} AS neighbor, "
                    "{native_id: r.localId, uuid: r.uuid, type: type(r), "
                    " start_native_id: startNode(r).localId, start_uuid: startNode(r).uuid, "
                    " start_is_proxy: coalesce(startNode(r).is_proxy,false), "
                    " end_native_id: endNode(r).localId, end_uuid: endNode(r).uuid, "
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

            std::unordered_map<TypeId, json> native_node_groups;
            json proxy_rows = json::array();
            std::unordered_map<TypeId, json> native_edge_groups;
            std::unordered_map<TypeId, json> replica_edge_groups;

            for (const insertion::InsertionItem &item: query.items()) {
                if (item.is_node()) {
                    const Node &node = item.node();
                    if (home_backend(node.global_id) == NEO4J) {
                        json &rows = native_node_groups[node.type];
                        if (!rows.is_array()) {
                            rows = json::array();
                        }
                        rows.push_back(native_node_to_write_row(node));
                    } else {
                        proxy_rows.push_back(proxy_node_to_write_row(node));
                    }
                    continue;
                }

                const Edge &edge = item.edge();
                auto &groups = home_backend(edge.global_id) == NEO4J ? native_edge_groups : replica_edge_groups;
                json &rows = groups[edge.type];
                if (!rows.is_array()) {
                    rows = json::array();
                }
                rows.push_back(home_backend(edge.global_id) == NEO4J ? native_edge_to_write_row(edge)
                                                                     : replica_edge_to_write_row(edge));
            }

            for (const auto &[node_type, rows]: native_node_groups) {
                const std::string label = escape_cypher_identifier(db_context_.schema->node_type(node_type).name);
                const std::string cypher = "UNWIND $rows AS row "
                                           "MERGE (n:POLYGRAPHVertex {localId: row.local_id}) "
                                           "SET n:" +
                                           label +
                                           ":__GraphNode "
                                           "SET n = row.properties "
                                           "SET n.localId = row.local_id, n.type = row.type, "
                                           "    n.source = 'Neo4J', n.is_proxy = false";
                execute_write(cypher, {{"rows", rows}});
            }

            if (!proxy_rows.empty()) {
                const std::string cypher = "UNWIND $rows AS row "
                                           "MERGE (n:Proxy:POLYGRAPHVertex:__GraphNode {uuid: row.uuid}) "
                                           "SET n = row.properties "
                                           "SET n.uuid = row.uuid, n.type = 'Proxy', n.is_proxy = true, "
                                           "    n.`remote-type` = row.remote_type";
                execute_write(cypher, {{"rows", proxy_rows}});
            }

            const auto insert_edges = [this](const std::unordered_map<TypeId, json> &groups, const bool native) {
                for (const auto &[edge_type, rows]: groups) {
                    const std::string relationship =
                            escape_cypher_identifier(db_context_.schema->edge_type(edge_type).name);
                    const std::string id_assignment = native ? "SET r.localId = row.local_id, r.type = row.type"
                                                             : "SET r.uuid = row.uuid, r.type = row.type";
                    // NOLINTBEGIN(performance-inefficient-string-concatenation)
                    const std::string cypher = "UNWIND $rows AS row "
                                               "MATCH (source) WHERE "
                                               "  (row.src_proxy AND source:Proxy AND source.uuid = row.src) OR "
                                               "  (NOT row.src_proxy AND source.localId = row.src) "
                                               "MATCH (target) WHERE "
                                               "  (row.dst_proxy AND target:Proxy AND target.uuid = row.dst) OR "
                                               "  (NOT row.dst_proxy AND target.localId = row.dst) "
                                               "CREATE (source)-[r:" +
                                               relationship +
                                               "]->(target) "
                                               "SET r = row.properties " +
                                               id_assignment;
                    // NOLINTEND(performance-inefficient-string-concatenation)
                    execute_write(cypher, {{"rows", rows}});
                }
            };

            insert_edges(native_edge_groups, true);
            insert_edges(replica_edge_groups, false);
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
                                         std::size_t request_retries, std::size_t retry_delay_ms,
                                         const BackendRequestScheduling &scheduling) const {
            /*
             * Keep all three calls together so benchmarking/switching requires
             * changing only these lines. The rich-row implementation is retained
             * as the third option.
             */
            // return get_minimal_neighbors_batch_direct_union(frontier, spec, depth);
            return get_minimal_neighbors_batch_direct_grouped_async(frontier, spec, depth, batch_size,
                                                                    max_concurrent_requests, request_retries,
                                                                    retry_delay_ms, scheduling);
            // return adjacency_rows_to_minimal(get_neighbors_batch_fast(frontier, spec, depth));
        }

    private:
        [[nodiscard]] GlobalId neo4j_local_id(const GlobalId id) const {
            return insertion::BackendEncodedId::local_id(id, db_context_.id_routing_backends);
        }

        [[nodiscard]] GlobalId neo4j_local_global_id(const json &value, const json &residency = json(0)) const {
            if (!value.is_number_integer() && !value.is_number_unsigned()) {
                throw std::runtime_error("Neo4j localId must be an integer: " + value.dump());
            }
            insertion::BackendEncodedId::LocationMask cache_mask = 0U;
            if (residency.is_number_integer() || residency.is_number_unsigned()) {
                cache_mask = static_cast<insertion::BackendEncodedId::LocationMask>(residency.get<std::uint64_t>());
            }
            return insertion::BackendEncodedId::from_local_id(value.get<std::uint64_t>(), NEO4J,
                                                              db_context_.id_routing_backends, cache_mask);
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
            return neo4j_local_global_id(native_id);
        }

        [[nodiscard]] GlobalId resolve_edge_global_id(const json &native_id, const json &stored_id) const {
            return stored_id.is_null() ? neo4j_local_global_id(native_id) : stored_global_id(stored_id);
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
                    const auto canonical = insertion::BackendEncodedId::canonical_id(source.global_id);
                    if (canonical != source.global_id) {
                        row_by_source.emplace(canonical, rows.size() - 1);
                    }
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

                const std::string query = source_lookup("frontier", source_label) +
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
                    const auto canonical = insertion::BackendEncodedId::canonical_id(source.global_id);
                    if (canonical != source.global_id) {
                        row_by_source.emplace(canonical, rows.size() - 1);
                    }
                }
            }

            const auto grouped_frontier = group_frontier_by_physical_source_label(frontier);
            const auto edge_type_names = edge_type_names_for_spec(spec);

            EVENT_SCOPE_INFO_FIELD(total_span, "label_group_count", grouped_frontier.size());

            std::vector<std::future<Neo4jGroupResponse>> futures;

            for (const auto &[source_label, group]: grouped_frontier) {
                if (group.empty()) {
                    continue;
                }

                json frontier_json = json::array();

                for (const auto &source: group) {
                    frontier_json.push_back(neo4j_local_id(source.global_id));
                }

                const std::string query = source_lookup("frontier", source_label) +
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

            EVENT_SCOPE_INFO_FIELD(total_span, "result_source_rows", rows.size());

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

                // Keep native and proxy lookups in separate UNION branches. Both
                // branches start from a unique indexed property and therefore
                // avoid OR predicates, label scans and Cartesian products.
                for (int source_kind = 0; source_kind < 2; ++source_kind) {
                    const bool source_is_proxy = source_kind == 1;

                    std::vector<NodeRef> physical_group;
                    physical_group.reserve(group.size());

                    for (const NodeRef &source: group) {
                        const bool is_proxy = home_backend(source.global_id) != NEO4J;
                        if (is_proxy == source_is_proxy) {
                            physical_group.push_back(source);
                        }
                    }

                    if (physical_group.empty()) {
                        continue;
                    }

                    if (!query.empty()) {
                        query += " UNION ALL ";
                    }

                    const std::string parameter_name = "frontier_" + std::to_string(branch_index++);

                    parameters[parameter_name] = routed_frontier_rows(physical_group, source_is_proxy);
                    parameters[parameter_name + "_source_label"] = source_label;

                    query += routed_source_lookup(parameter_name, source_label, source_is_proxy) + edge_match;

                    if (!target_type_names.empty()) {
                        // source_item carries the original global source ID and
                        // must remain visible to minimal_neighbor_projection().
                        query += "WITH source_item, source, neighbor "
                                 "WHERE (neighbor:Proxy AND neighbor.`remote-type` IN $target_types) "
                                 "   OR (NOT neighbor:Proxy AND neighbor.type IN $target_types) ";
                    }

                    query += minimal_neighbor_projection();
                }
            }

            if (query.empty()) {
                return rows;
            }

            const json response = send_req(query, parameters);
            parse_minimal_response_into_rows(response, rows, row_by_source, spec);

            EVENT_SCOPE_INFO_FIELD(total_span, "result_source_rows", rows.size());
            return rows;
        }

        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_direct_grouped_async(
                std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth, std::size_t batch_size,
                std::size_t max_concurrent_requests, std::size_t request_retries, std::size_t retry_delay_ms,
                const BackendRequestScheduling &scheduling) const {
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

            struct Neo4jFailedBatch {
                std::size_t prepared_index{};
                std::string error;
                bool transient{};
            };

            struct Neo4jWorkerStats {
                std::size_t completed_requests{};
                std::size_t response_row_count{};
                std::vector<Neo4jFailedBatch> failures;
            };

            std::vector<Neo4jPreparedBatch> prepared_batches;

            for (const auto &[source_label, group]: grouped_frontier) {
                if (group.empty()) {
                    continue;
                }

                std::string query = combined_routed_source_lookup(source_label) + edge_match;

                if (!target_type_names.empty()) {
                    query += "WITH source_item, source, neighbor "
                             "WHERE (neighbor:Proxy AND neighbor.`remote-type` IN $target_types) "
                             "   OR (NOT neighbor:Proxy AND neighbor.type IN $target_types) ";
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

                    json native_frontier = json::array();
                    json proxy_frontier = json::array();
                    auto &native_rows = native_frontier.get_ref<json::array_t &>();
                    auto &proxy_rows = proxy_frontier.get_ref<json::array_t &>();
                    native_rows.reserve(current_batch_size);
                    proxy_rows.reserve(current_batch_size);

                    for (const NodeRef &source: batch_nodes) {
                        const bool source_is_proxy = home_backend(source.global_id) != NEO4J;
                        auto &target_rows = source_is_proxy ? proxy_rows : native_rows;
                        target_rows.push_back(routed_frontier_row(source, source_is_proxy));
                    }

                    prepared_batches.push_back(
                            Neo4jPreparedBatch{.source_label = source_label,
                                               .batch_index = batch_index,
                                               .batch_count = batch_count,
                                               .nodes = std::move(batch_nodes),
                                               .query = query,
                                               .parameters = json{
                                                       {"native_frontier", std::move(native_frontier)},
                                                       {"proxy_frontier", std::move(proxy_frontier)},
                                                       {"source_label", source_label},
                                                       {"target_types", target_type_names},
                                               }});
                }
            }

            if (prepared_batches.empty()) {
                return rows;
            }

            std::vector<std::size_t> pending_batches;
            pending_batches.reserve(prepared_batches.size());
            for (std::size_t index = 0; index < prepared_batches.size(); ++index) {
                pending_batches.push_back(index);
            }

            std::size_t current_concurrency =
                    std::min({max_concurrent_requests, pending_batches.size(), scheduler_->config().max_in_flight});
            std::size_t recovery_round = 0;
            std::size_t completed_requests = 0;
            std::size_t total_response_rows = 0;
            std::mutex rows_mutex;

            while (!pending_batches.empty()) {
                const std::size_t worker_count = std::min(current_concurrency, pending_batches.size());
                std::atomic_size_t next_pending{0};
                std::vector<std::future<Neo4jWorkerStats>> workers;
                workers.reserve(worker_count);

                for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
                    workers.push_back(std::async(
                            std::launch::async,
                            [this, &prepared_batches, &pending_batches, &next_pending, &rows, &row_by_source,
                             &rows_mutex, spec, request_retries, retry_delay_ms, worker_index, current_concurrency,
                             recovery_round, scheduling]() mutable -> Neo4jWorkerStats {
                                Neo4jWorkerStats stats;
                                std::unique_ptr<Neo4JBoltClient> bolt_client;

                                while (true) {
                                    const std::size_t pending_index =
                                            next_pending.fetch_add(1, std::memory_order_relaxed);
                                    if (pending_index >= pending_batches.size()) {
                                        break;
                                    }

                                    const std::size_t prepared_index = pending_batches[pending_index];
                                    const auto &prepared = prepared_batches[prepared_index];
                                    const std::size_t current_batch_size = prepared.nodes.size();
                                    EVENT_SCOPE_INFO(
                                            request_span, "neo4j.minimal_neighbors.direct_group_query",
                                            {"source_label", prepared.source_label}, {"group_size", current_batch_size},
                                            {"batch_index", prepared.batch_index},
                                            {"batch_count", prepared.batch_count}, {"worker_index", worker_index},
                                            {"adaptive_concurrency", current_concurrency},
                                            {"recovery_round", recovery_round});

                                    for (std::size_t attempt = 0;; ++attempt) {
                                        try {
                                            if (use_bolt_ && !bolt_client) {
                                                bolt_client = make_bolt_client();
                                            }
                                            auto [batch_rows, batch_row_by_source] =
                                                    initialize_minimal_rows(prepared.nodes);
                                            const std::size_t response_row_count = scheduler_->run(
                                                    scheduling, use_bolt_ ? "neo4j.bolt" : "neo4j.http", [&] {
                                                        return send_minimal_req_into_rows(
                                                                prepared.query, prepared.parameters, batch_rows,
                                                                batch_row_by_source, spec, prepared.source_label,
                                                                current_batch_size, bolt_client.get());
                                                    });

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
                                                EVENT_SCOPE_INFO_FIELD(response_to_rows, "result_source_rows",
                                                                       rows.size());
                                            }

                                            EVENT_SCOPE_INFO_FIELD(request_span, "attempts", attempt + 1);
                                            EVENT_SCOPE_INFO_FIELD(request_span, "response_row_count",
                                                                   response_row_count);
                                            EVENT_SCOPE_INFO_FIELD(request_span, "status", "ok");
                                            ++stats.completed_requests;
                                            stats.response_row_count += response_row_count;
                                            break;
                                        } catch (const std::exception &error) {
                                            const bool transient = is_transient_backend_error(error.what());

                                            // At high concurrency, immediately defer a transiently failed batch
                                            // to the next round instead of having every worker hammer Neo4j with
                                            // synchronized retries. The next round halves concurrency. At a single
                                            // request, use the configured retry count and exponential backoff.
                                            const bool defer_to_lower_concurrency =
                                                    transient && current_concurrency > 1;
                                            if (defer_to_lower_concurrency || !transient ||
                                                attempt >= request_retries) {
                                                if (transient && use_bolt_) {
                                                    bolt_client.reset();
                                                }
                                                EVENT_SCOPE_INFO_FIELD(request_span, "attempts", attempt + 1);
                                                EVENT_SCOPE_INFO_FIELD(request_span, "status",
                                                                       defer_to_lower_concurrency ? "deferred"
                                                                                                  : "failed");
                                                EVENT_SCOPE_INFO_FIELD(request_span, "error", error.what());
                                                stats.failures.push_back(
                                                        Neo4jFailedBatch{.prepared_index = prepared_index,
                                                                         .error = error.what(),
                                                                         .transient = transient});
                                                break;
                                            }

                                            const std::size_t delay =
                                                    retry_delay_ms *
                                                    (std::size_t{1} << std::min(attempt, std::size_t{10}));
                                            const std::size_t jitter = retry_delay_ms == 0 ? 0 : (worker_index % 8) * 7;
                                            EVENT_SCOPE_INFO(retry_span, "neo4j.minimal_neighbors.request_retry",
                                                             {"source_label", prepared.source_label},
                                                             {"batch_index", prepared.batch_index},
                                                             {"attempt", attempt + 1}, {"delay_ms", delay + jitter},
                                                             {"error", error.what()});
                                            if (use_bolt_) {
                                                bolt_client.reset();
                                            }
                                            std::this_thread::sleep_for(std::chrono::milliseconds(delay + jitter));
                                        }
                                    }
                                }

                                return stats;
                            }));
                }

                std::vector<Neo4jFailedBatch> failed_batches;
                for (auto &worker: workers) {
                    Neo4jWorkerStats stats = worker.get();
                    completed_requests += stats.completed_requests;
                    total_response_rows += stats.response_row_count;
                    failed_batches.insert(failed_batches.end(), std::make_move_iterator(stats.failures.begin()),
                                          std::make_move_iterator(stats.failures.end()));
                }

                if (failed_batches.empty()) {
                    pending_batches.clear();
                    break;
                }

                const auto terminal_failure =
                        std::find_if(failed_batches.begin(), failed_batches.end(),
                                     [](const Neo4jFailedBatch &failure) { return !failure.transient; });
                if (terminal_failure != failed_batches.end() || current_concurrency == 1) {
                    const Neo4jFailedBatch &failure =
                            terminal_failure != failed_batches.end() ? *terminal_failure : failed_batches.front();
                    const auto &prepared = prepared_batches[failure.prepared_index];
                    throw std::runtime_error("Neo4j neighbor batch " + std::to_string(prepared.batch_index + 1) + "/" +
                                             std::to_string(prepared.batch_count) + " for label '" +
                                             prepared.source_label +
                                             "' failed after adaptive concurrency was reduced to " +
                                             std::to_string(current_concurrency) + ": " + failure.error);
                }

                pending_batches.clear();
                pending_batches.reserve(failed_batches.size());
                for (const Neo4jFailedBatch &failure: failed_batches) {
                    pending_batches.push_back(failure.prepared_index);
                }

                current_concurrency = std::max(std::size_t{1}, current_concurrency / 2);
                ++recovery_round;
                const std::size_t recovery_delay =
                        retry_delay_ms * (std::size_t{1} << std::min(recovery_round - 1, std::size_t{10}));
                EVENT_SCOPE_INFO(recovery_span, "neo4j.minimal_neighbors.adaptive_backpressure",
                                 {"failed_request_count", pending_batches.size()},
                                 {"next_concurrency", current_concurrency}, {"recovery_round", recovery_round},
                                 {"delay_ms", recovery_delay});
                if (recovery_delay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(recovery_delay));
                }
            }

            EVENT_SCOPE_INFO_FIELD(total_span, "request_count", prepared_batches.size());
            EVENT_SCOPE_INFO_FIELD(total_span, "completed_request_count", completed_requests);
            EVENT_SCOPE_INFO_FIELD(total_span, "response_row_count", total_response_rows);
            EVENT_SCOPE_INFO_FIELD(total_span, "recovery_round_count", recovery_round);
            EVENT_SCOPE_INFO_FIELD(total_span, "final_concurrency", current_concurrency);
            EVENT_SCOPE_INFO_FIELD(total_span, "result_source_rows", rows.size());
            return rows;
        }

        static std::string rich_neighbor_projection() {
            return "RETURN "
                   "source.localId, neighbor.localId, neighbor.uuid, "
                   "coalesce(neighbor.is_proxy, false), neighbor.`remote-type`, "
                   "neighbor.type, labels(neighbor), "
                   "r.localId, r.uuid, type(r), "
                   "startNode(r).localId, startNode(r).uuid, coalesce(startNode(r).is_proxy, false), "
                   "endNode(r).localId, endNode(r).uuid, coalesce(endNode(r).is_proxy, false), "
                   "coalesce(neighbor.cache_residency, 0)";
        }

        static std::string minimal_neighbor_projection() {
            return "RETURN "
                   "source_item.source_global_id AS source_global_id, "
                   "CASE WHEN neighbor:Proxy THEN neighbor.uuid ELSE neighbor.localId END AS neighbor_id, "
                   "neighbor:Proxy AS neighbor_is_proxy, "
                   "CASE WHEN neighbor:Proxy THEN neighbor.`remote-type` ELSE neighbor.type END AS neighbor_type, "
                   "CASE WHEN neighbor:Proxy THEN 0 ELSE coalesce(neighbor.cache_residency, 0) END AS cache_residency";
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
                    const auto canonical = insertion::BackendEncodedId::canonical_id(source.global_id);
                    if (canonical != source.global_id) {
                        row_by_source.emplace(canonical, rows.size() - 1);
                    }
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
            if (!row.is_array() || row.size() < 4 || row[0].is_null() || row[1].is_null() || !row[2].is_boolean() ||
                !row[3].is_string()) {
                return;
            }

            const auto source_id = stored_global_id(row[0]);
            const bool neighbor_is_proxy = row[2].get<bool>();
            const auto neighbor_id = neighbor_is_proxy
                                             ? stored_global_id(row[1])
                                             : neo4j_local_global_id(row[1], row.size() > 4 ? row[4] : json(0));
            const std::string neighbor_type_name = row[3].get<std::string>();
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

        [[nodiscard]] json routed_frontier_row(const NodeRef &source, const bool source_is_proxy) const {
            return json{
                    {"lookup_id", source_is_proxy ? json(global_id_to_signed_storage(source.global_id))
                                                  : json(neo4j_local_id(source.global_id))},
                    {"source_global_id", global_id_to_signed_storage(source.global_id)},
            };
        }

        [[nodiscard]] json routed_frontier_rows(const std::vector<NodeRef> &sources, const bool source_is_proxy) const {
            json result = json::array();
            auto &rows = result.get_ref<json::array_t &>();
            rows.reserve(sources.size());

            for (const NodeRef &source: sources) {
                rows.push_back(routed_frontier_row(source, source_is_proxy));
            }

            return result;
        }

        static std::string routed_source_lookup(const std::string &frontier_parameter, const std::string &source_label,
                                                const bool source_is_proxy) {
            if (source_is_proxy) {
                return "UNWIND $" + frontier_parameter +
                       " AS source_item "
                       "MATCH (source:Proxy {uuid: source_item.lookup_id}) "
                       "WHERE source.`remote-type` = $" +
                       frontier_parameter + "_source_label ";
            }

            return "UNWIND $" + frontier_parameter +
                   " AS source_item "
                   "MATCH (source:POLYGRAPHVertex {localId: source_item.lookup_id}) "
                   "WHERE source:" +
                   escape_cypher_identifier(source_label) + " ";
        }

        static std::string combined_routed_source_lookup(const std::string &source_label) {
            return "CALL { "
                   "UNWIND $native_frontier AS source_item "
                   "MATCH (source:POLYGRAPHVertex {localId: source_item.lookup_id}) "
                   "WHERE source:" +
                   escape_cypher_identifier(source_label) +
                   " RETURN source_item, source "
                   "UNION ALL "
                   "UNWIND $proxy_frontier AS source_item "
                   "MATCH (source:Proxy {uuid: source_item.lookup_id}) "
                   "WHERE source.`remote-type` = $source_label "
                   "RETURN source_item, source "
                   "} ";
        }

        static std::string source_lookup(const std::string &frontier_parameter, const std::string &source_label) {
            return "UNWIND $" + frontier_parameter +
                   " AS source_native_id "
                   "MATCH (source:POLYGRAPHVertex {localId: source_native_id}) "
                   "WHERE source:" +
                   escape_cypher_identifier(source_label) + " ";
        }

        static bool is_transient_backend_error(const std::string &message) {
            static const std::vector<std::string> transient_markers{
                    "Server returned nothing",
                    "Empty reply from server",
                    "Couldn't connect",
                    "Failed to connect",
                    "Connection reset",
                    "Connection refused",
                    "Broken pipe",
                    "timed out",
                    "Timeout",
                    "status 408",
                    "status 429",
                    "status 502",
                    "status 503",
                    "status 504",
                    "ServiceUnavailable",
                    "SessionExpired",
                    "TransientError",
            };

            return std::any_of(
                    transient_markers.begin(), transient_markers.end(),
                    [&message](const std::string &marker) { return message.find(marker) != std::string::npos; });
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
            return label == "Proxy" || label == "POLYGRAPHVertex" || label == "__GraphNode";
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

                if (r.size() < 16 || r[0].is_null()) {
                    continue;
                }

                const bool looks_like_proxy = bool_or_false(r[3]) ||
                                              non_empty_string_opt(r[5]).value_or("") == "Proxy" ||
                                              labels_contain(r[6], "Proxy");
                if ((looks_like_proxy && r[2].is_null()) || (!looks_like_proxy && r[1].is_null())) {
                    continue;
                }
                const auto source_id = neo4j_local_global_id(r[0]);
                const auto neighbor_id = looks_like_proxy
                                                 ? resolve_node_global_id(r[1], r[2], true)
                                                 : neo4j_local_global_id(r[1], r.size() > 16 ? r[16] : json(0));
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

        [[nodiscard]] json native_node_to_write_row(const Node &node) const {
            json properties = node.properties;
            properties.erase("uuid");
            properties.erase("localId");
            return json{{"local_id", neo4j_local_id(node.global_id)},
                        {"type", db_context_.schema->node_type(node.type).name},
                        {"properties", std::move(properties)}};
        }

        [[nodiscard]] json proxy_node_to_write_row(const Node &node) const {
            json properties = node.properties;
            properties.erase("uuid");
            properties.erase("localId");
            return json{{"uuid", global_id_to_signed_storage(node.global_id)},
                        {"remote_type", db_context_.schema->node_type(node.type).name},
                        {"properties", std::move(properties)}};
        }

        [[nodiscard]] json edge_to_write_row(const Edge &edge) const {
            json properties = edge.properties;
            properties.erase("uuid");
            properties.erase("localId");
            return json{{"type", edge.type},
                        {"src", endpoint_storage_id(edge.src)},
                        {"dst", endpoint_storage_id(edge.dst)},
                        {"src_proxy", home_backend(edge.src) != NEO4J},
                        {"dst_proxy", home_backend(edge.dst) != NEO4J},
                        {"properties", std::move(properties)}};
        }

        [[nodiscard]] json native_edge_to_write_row(const Edge &edge) const {
            json row = edge_to_write_row(edge);
            row["local_id"] = neo4j_local_id(edge.global_id);
            return row;
        }

        [[nodiscard]] json replica_edge_to_write_row(const Edge &edge) const {
            json row = edge_to_write_row(edge);
            row["uuid"] = global_id_to_signed_storage(edge.global_id);
            return row;
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
                EVENT_SCOPE_INFO_FIELD(bolt_span, "response_row_count", response_row_count);
                return response_row_count;
            }

            EVENT_SCOPE_INFO(total_span, "neo4j.bfs_http_query", {"source_label", source_label},
                             {"group_size", group_size}, {"streaming_minimal_rows", true});

            json request;
            {
                EVENT_SCOPE_INFO(request_span, "neo4j.bfs_req_construct", {"source_label", source_label},
                                 {"group_size", group_size});
                request = json{{"statements", json::array({{{"statement", query}, {"parameters", parameters}}})}};
                EVENT_SCOPE_INFO_FIELD(request_span, "query_bytes", query.size());
            }

            std::string request_body;
            {
                EVENT_SCOPE_INFO(dump_span, "neo4j.bfs_json_req_dump", {"source_label", source_label});
                request_body = request.dump();
                EVENT_SCOPE_INFO_FIELD(dump_span, "request_bytes", request_body.size());
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
                EVENT_SCOPE_INFO_FIELD(http_span, "response_bytes", response_body.size());
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
                            parsed.is_array() && parsed.size() >= 4 && parsed[0].is_number_integer() &&
                            parsed[1].is_number_integer() && parsed[2].is_boolean() && parsed[3].is_string();

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
                EVENT_SCOPE_INFO_FIELD(parse_span, "response_row_count", response_row_count);
            }

            if (metadata.contains("errors") && metadata["errors"].is_array() && !metadata["errors"].empty()) {
                throw std::runtime_error("Neo4J minimal neighbor query failed: " + metadata["errors"].dump());
            }

            if (!metadata.contains("results") || !metadata["results"].is_array() || metadata["results"].empty() ||
                !metadata["results"][0].contains("data")) {
                throw std::runtime_error("Neo4J minimal neighbor response has no results[0].data: " + metadata.dump());
            }

            EVENT_SCOPE_INFO_FIELD(total_span, "response_row_count", response_row_count);
            EVENT_SCOPE_INFO_FIELD(total_span, "status", "ok");
            return response_row_count;
        }

        json send_req(const std::string &query, const json &parameters) const {
            // POLYGRAPH_PROFILE_FUNCTION();

            if (use_bolt_) {
                // POLYGRAPH_PROFILE_SCOPE("Neo4J send_req: Bolt RUN/PULL");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_bolt_query");
                return send_req_bolt(query, parameters);
            }
            EVENT_SCOPE_INFO(_, "neo4j.bfs_http_query");

            json req;

            {
                // POLYGRAPH_PROFILE_SCOPE("Neo4J send_req: build request JSON");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_req_construct");
                req = json{{"statements", json::array({{{"statement", query}, {"parameters", parameters}}})}};
            }

            std::string request_body;

            {
                // POLYGRAPH_PROFILE_SCOPE("Neo4J send_req: dump request JSON");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_json_req_dump");
                request_body = req.dump();
            }

            std::string response_body;
            std::string auth;

            if (config_.credentials.has_value()) {
                auth = config_.credentials->username + ":" + config_.credentials->password;
            }

            {
                // POLYGRAPH_PROFILE_SCOPE("Neo4J send_req: HTTP POST");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_http_post");
                response_body = http_post(config_.endpoint, request_body, auth);
            }

            json parsed;

            {
                // POLYGRAPH_PROFILE_SCOPE("Neo4J send_req: parse response JSON");
                EVENT_SCOPE_INFO(_, "neo4j.bfs_response_to_json_parse");
                parsed = json::parse(response_body);
            }

            EVENT_SCOPE_INFO(_a, "neo4j.debug");
            return parsed;
        }

        static bool should_use_bolt(const BackendConnectionConfig &config) {
            if (const char *env = std::getenv("POLYGRAPH_NEO4J_TRANSPORT")) {
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
            // POLYGRAPH_PROFILE_FUNCTION();

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

    Neo4JQueryVisitor::Neo4JQueryVisitor(DatabaseContext &database_context, const BackendConnectionConfig &config,
                                         std::shared_ptr<BackendRequestScheduler> scheduler) :
        QueryVisitor(database_context), impl_(std::make_unique<Impl>(database_context, config, std::move(scheduler))) {}

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
            std::size_t max_concurrent_requests, std::size_t request_retries, std::size_t retry_delay_ms,
            const BackendRequestScheduling &scheduling) const {
        return impl_->get_minimal_neighbors_batch_fast(frontier, spec, depth, batch_size, max_concurrent_requests,
                                                       request_retries, retry_delay_ms, scheduling);
    }

} // namespace polygraph
