/**
 * @file src/data_access/adapters/ArangoDB/ArangoQueryVisitor.cpp
 * @brief Implementation for arango query visitor in the graph-store access module.
 */

#include "data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp"

#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "logging/level/Debug.hpp"
#include "logging/level/Info.hpp"
#include "query/ResultParseType.hpp"
#include "query/query_types/GetNeighborsBatchQuery.hpp"
#include "query/query_types/GetNeighborsForVertexQuery.hpp"
#include "query/query_types/GetVertexByIdQuery.hpp"
#include "query/query_types/InsertBatchQuery.hpp"
#include "query/query_types/InsertQuery.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/Profiling.hpp"
#include "utilities/logging.hpp"

#include <boost/container_hash/hash.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid_io.hpp>
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

namespace cpgdb {

    struct ArangoQueryVisitor::Impl final : QueryVisitor {
        const BackendConnectionConfig &config_;
        DataModelJsonSerializer serializer_;
        mutable CurlHttpClient http_;
        mutable std::mutex collection_cache_mutex_;
        mutable std::optional<std::unordered_set<std::string>> node_collection_cache_;
        mutable std::optional<std::unordered_set<std::string>> edge_collection_cache_;

        enum class collection_t : std::size_t { NODE = 2, EDGE = 3 };

    public:
        Impl(DatabaseContext &db_context, const BackendConnectionConfig &config) :
            QueryVisitor(db_context), config_(config),
            http_(config.credentials.has_value() ? config.credentials->username + ":" + config.credentials->password
                                                 : "") {}

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

            const std::string query = "FOR doc IN @@collection "
                                      "FILTER doc.uuid == @uuid "
                                      "LIMIT 1 "
                                      "RETURN doc";

            json bindVars = {{"@collection", collection}, {"uuid", boost::uuids::to_string(node.global_id)}};

            return evaluate(query, bindVars, ResultParseType::Node);
        }

        query_result_t visit(const GetNeighborsBatchQuery &q) override {
            // CPGDB_PROFILE_FUNCTION();

            EVENT_SCOPE_INFO(_, "arango.neighbor_query", {"frontier_size", q.frontier().size()});
            auto grouped_frontier = group_frontier_by_collection(q.frontier());

            auto combined = std::make_unique<QueryResult>();

            for (const auto &[collection, frontier]: grouped_frontier) {
                if (frontier.empty()) {
                    continue;
                }

                json frontierJson = json::array();

                for (const auto &v: frontier) {
                    frontierJson.push_back(boost::uuids::to_string(v.global_id));
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
                const std::string query = "FOR start IN @@vertexCollection "
                                          "FILTER start._key IN @frontier "
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

        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch_fast(std::span<const NodeRef> frontier,
                                                                          const ExpandSpec &spec, int depth) const {
            /*
             * Keep the switch here: one active implementation and the other
             * two preserved immediately below it for reproducible benchmarks.
             */
            // return get_minimal_neighbors_batch_edge_index_union(frontier, spec, depth);
            return get_minimal_neighbors_batch_traversal_projection(frontier, spec, depth);
            // return get_minimal_neighbors_batch_original(frontier, spec, depth);
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
                const std::string query_text = "LET inserted = ("
                                               "  FOR row IN @rows "
                                               "    INSERT MERGE(row.properties, {"
                                               "      uuid: row.uuid, cpgdb_type_id: row.type, labels: row.labels"
                                               "    }) INTO @@collection "
                                               "    RETURN 1"
                                               ") RETURN LENGTH(inserted)";
                const json bind_vars{{"rows", rows}, {"@collection", db_context_.schema->node_type(node_type).name}};
                execute_write(query_text, bind_vars, rows.size());
            }

            for (const auto &[edge_type, rows]: edge_groups) {
                const auto &schema_edge = db_context_.schema->edge_type(edge_type);
                const std::string query_text =
                        "LET inserted = ("
                        "  FOR row IN @rows "
                        "    LET source = FIRST("
                        "      FOR doc IN @@sourceCollection FILTER doc.uuid == row.src LIMIT 1 RETURN doc"
                        "    ) "
                        "    LET target = FIRST("
                        "      FOR doc IN @@targetCollection FILTER doc.uuid == row.dst LIMIT 1 RETURN doc"
                        "    ) "
                        "    FILTER source != null AND target != null "
                        "    INSERT MERGE(row.properties, {"
                        "      uuid: row.uuid, cpgdb_type_id: row.type,"
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
                    frontier_json.push_back(boost::uuids::to_string(source.global_id));
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
                                    "  FOR start IN @@vertexCollection "
                                    "    FILTER start._key IN @frontier "
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
                    frontier_json.push_back(boost::uuids::to_string(source.global_id));
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

                const std::string query = "FOR start IN @@vertexCollection "
                                          "FILTER start._key IN @frontier "
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

            std::unordered_map<GlobalId, std::unordered_set<GlobalId, boost::hash<GlobalId>>, boost::hash<GlobalId>>
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
                         std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>>>
        initialize_minimal_rows(std::span<const NodeRef> frontier) {
            std::vector<MinimalAdjacencyRow> rows;
            rows.reserve(frontier.size());

            std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>> row_by_source;
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

        void parse_minimal_response_into_rows(
                const json &response, std::vector<MinimalAdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>> &row_by_source,
                const ExpandSpec &spec) const {
            if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                throw std::runtime_error("ArangoDB minimal neighbor query failed: " + response.dump());
            }

            if (!response.contains("result") || !response["result"].is_array()) {
                throw std::runtime_error("ArangoDB minimal neighbor response has no result array: " + response.dump());
            }

            std::unordered_map<GlobalId, std::unordered_set<GlobalId, boost::hash<GlobalId>>, boost::hash<GlobalId>>
                    seen_by_source;

            for (const auto &item: response["result"]) {
                append_minimal_result_item(item, rows, row_by_source, seen_by_source, spec);
            }
        }

        void append_minimal_result_item(
                const json &item, std::vector<MinimalAdjacencyRow> &rows,
                const std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>> &row_by_source,
                std::unordered_map<GlobalId, std::unordered_set<GlobalId, boost::hash<GlobalId>>, boost::hash<GlobalId>>
                        &seen_by_source,
                const ExpandSpec &spec) const {
            if (!item.is_array()) {
                return;
            }

            const bool looks_like_row =
                    item.size() >= 4 && item[0].is_string() && item[1].is_string() && item[2].is_string();

            if (!looks_like_row) {
                for (const auto &nested: item) {
                    append_minimal_result_item(nested, rows, row_by_source, seen_by_source, spec);
                }
                return;
            }

            const auto source_id = boost::lexical_cast<GlobalId>(item[0].get<std::string>());
            const auto neighbor_id = boost::lexical_cast<GlobalId>(item[1].get<std::string>());
            const std::string neighbor_document_id = item[2].get<std::string>();
            std::string logical_type_name = collection_from_arango_id(neighbor_document_id);

            if (logical_type_name == "Proxy") {
                if (item[3].is_null() || !item[3].is_string() || item[3].get<std::string>().empty()) {
                    throw std::runtime_error("ArangoDB proxy neighbor is missing remote-type: " + item.dump());
                }

                logical_type_name = item[3].get<std::string>();
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
                // UUID suffix bits identify ArangoDB, but not the document
                // collection. Resolve only the initial type-free frontier by
                // probing the materialized schema collections. Each probe uses
                // the per-collection primary _key index; no UUID-to-type
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
            // their UUID. Therefore an Arango-routed frontier vertex is a real
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
            std::scoped_lock lock(collection_cache_mutex_);
            if (!edge_collection_cache_.has_value()) {
                std::string auth;
                if (config_.credentials.has_value()) {
                    auth = config_.credentials->username + ":" + config_.credentials->password;
                }

                const auto collections = fetchAndSortCollections(collections_endpoint(), auth, collection_t::EDGE);
                edge_collection_cache_.emplace(collections.begin(), collections.end());
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
            return json{{"uuid", boost::uuids::to_string(node.global_id)},
                        {"type", node.type},
                        {"labels", node.labels},
                        {"properties", node.properties}};
        }

        static json edge_to_write_row(const Edge &edge) {
            return json{{"uuid", boost::uuids::to_string(edge.global_id)},
                        {"type", edge.type},
                        {"src", boost::uuids::to_string(edge.src)},
                        {"dst", boost::uuids::to_string(edge.dst)},
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

        [[nodiscard]] json send_req(const std::string &query, const json &bindVars) const {
            json req{{"query", query},
                     {"bindVars", bindVars},
                     // Neighbor expansion can easily return thousands of rows for one
                     // BFS layer. ArangoDB cursors are paginated; using a larger batch
                     // reduces round-trips, and the loop below still handles hasMore.
                     {"batchSize", 1000000}};

            std::string res = http_.post(config_.endpoint, req.dump());

            json response = json::parse(res);

            if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                return response;
            }

            if (!response.contains("result") || !response["result"].is_array()) {
                return response;
            }

            json combined_result = response["result"];

            while (response.value("hasMore", false)) {
                if (!response.contains("id") || !response["id"].is_string()) {
                    throw std::runtime_error("ArangoDB cursor says hasMore=true but response has no cursor id: " +
                                             response.dump());
                }

                const std::string next_url = config_.endpoint + "/" + response["id"].get<std::string>();

                std::string next_res = http_.put(next_url, "{}");
                response = json::parse(next_res);

                if (response.contains("error") && response["error"].is_boolean() && response["error"].get<bool>()) {
                    return response;
                }

                if (!response.contains("result") || !response["result"].is_array()) {
                    throw std::runtime_error("ArangoDB cursor page does not contain result array: " + response.dump());
                }

                for (const auto &item: response["result"]) {
                    combined_result.push_back(item);
                }
            }

            response["result"] = std::move(combined_result);
            response["hasMore"] = false;
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

            const std::string uuid_str = row["uuid"].get<std::string>();

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

            return Node{.global_id = boost::lexical_cast<uuid>(uuid_str),
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

            const std::string uuid_str = row["uuid"].get<std::string>();

            /*
             * Same rule as nodes:
             * logical edge type comes from edge collection name, not row["type"].
             */
            const std::string logical_edge_type_name = arango_collection_from_document(row);

            std::string start_uuid;
            std::string end_uuid;

            if (row.contains("start") && row.contains("end")) {
                start_uuid = row["start"].get<std::string>();
                end_uuid = row["end"].get<std::string>();
            } else if (row.contains("_from") && row.contains("_to")) {
                start_uuid = uuid_from_arango_document_id(row["_from"].get<std::string>());
                end_uuid = uuid_from_arango_document_id(row["_to"].get<std::string>());
            } else {
                throw std::runtime_error("Arango edge row does not contain start/end or _from/_to: " + row.dump());
            }

            return Edge{.global_id = boost::lexical_cast<uuid>(uuid_str),
                        .type = db_context_.schema->edge_type_id(logical_edge_type_name),
                        .src = boost::lexical_cast<uuid>(start_uuid),
                        .dst = boost::lexical_cast<uuid>(end_uuid),
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

    ArangoQueryVisitor::ArangoQueryVisitor(DatabaseContext &database_context, const BackendConnectionConfig &config) :
        QueryVisitor(database_context), impl_(std::make_unique<Impl>(database_context, config)) {}

    ArangoQueryVisitor::~ArangoQueryVisitor() = default;
    query_result_t ArangoQueryVisitor::visit(const GetAllVerticesQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const GetAllEdgesQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const GetNeighborsForVertexQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const GetVertexByIdQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const GetNeighborsBatchQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const InsertQuery &query) { return impl_->visit(query); }
    query_result_t ArangoQueryVisitor::visit(const InsertBatchQuery &query) { return impl_->visit(query); }

    std::vector<MinimalAdjacencyRow>
    ArangoQueryVisitor::get_minimal_neighbors_batch_fast(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                         int depth) const {
        return impl_->get_minimal_neighbors_batch_fast(frontier, spec, depth);
    }

} // namespace cpgdb
