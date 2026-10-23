/**
 * @file src/data_access/adapters/PostgreSQL/PostgreSqlGraphStore.cpp
 * @brief PostgreSQL relational-frontier graph expansion implementation.
 */
#include "data_access/adapters/PostgreSQL/PostgreSqlGraphStore.hpp"

#include "events/Events.hpp"
#include "insertion/BackendEncodedUuid.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace polygraph {
    namespace {

        [[nodiscard]] std::pair<std::string, std::string> parse_endpoint(const std::string &endpoint) {
            std::string value = endpoint.empty() ? "127.0.0.1:5432" : endpoint;
            constexpr std::string_view postgresql_prefix = "postgresql://";
            constexpr std::string_view postgres_prefix = "postgres://";
            if (value.starts_with(postgresql_prefix)) {
                value.erase(0, postgresql_prefix.size());
            } else if (value.starts_with(postgres_prefix)) {
                value.erase(0, postgres_prefix.size());
            }

            if (const auto slash = value.find('/'); slash != std::string::npos) {
                value.resize(slash);
            }
            if (const auto at = value.rfind('@'); at != std::string::npos) {
                value.erase(0, at + 1U);
            }

            std::string host = value;
            std::string port = "5432";
            if (const auto colon = value.rfind(':'); colon != std::string::npos) {
                host = value.substr(0, colon);
                port = value.substr(colon + 1U);
            }
            if (host.empty()) {
                host = "127.0.0.1";
            }
            if (port.empty()) {
                port = "5432";
            }
            return {std::move(host), std::move(port)};
        }

        [[nodiscard]] std::int64_t to_postgres_id(const GlobalId id) noexcept {
            static_assert(sizeof(GlobalId) == sizeof(std::int64_t));
            return std::bit_cast<std::int64_t>(id);
        }

        [[nodiscard]] GlobalId from_postgres_id(const std::string_view text) {
            std::int64_t signed_value = 0;
            const auto *begin = text.data();
            const auto *end = begin + text.size();
            const auto [pointer, error] = std::from_chars(begin, end, signed_value);
            if (error != std::errc{} || pointer != end) {
                throw std::runtime_error("PostgreSQL returned an invalid BIGINT GlobalId");
            }
            return std::bit_cast<GlobalId>(signed_value);
        }

        void append_u16_be(std::string &out, const std::uint16_t value) {
            out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
            out.push_back(static_cast<char>(value & 0xFFU));
        }

        void append_u32_be(std::string &out, const std::uint32_t value) {
            out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
            out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
            out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
            out.push_back(static_cast<char>(value & 0xFFU));
        }

        void append_u64_be(std::string &out, const std::uint64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<char>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
            }
        }

        [[nodiscard]] std::string pq_error(PGconn *connection, std::string_view prefix) {
            std::string message(prefix);
            message += ": ";
            message += connection != nullptr ? PQerrorMessage(connection) : "no PostgreSQL connection";
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
                message.pop_back();
            }
            return message;
        }

    } // namespace

    PostgreSqlGraphStore::PostgreSqlGraphStore(BackendDescriptor descriptor, BackendConnectionConfig connection,
                                               const domain::LogicalGraphSchema &schema,
                                               const bool server_side_candidate_deduplication) :
        descriptor_(std::move(descriptor)), connection_config_(std::move(connection)), schema_(schema),
        server_side_candidate_deduplication_(server_side_candidate_deduplication) {
        node_type_ids_.reserve(schema_.node_types().size());
        for (const auto &[type_id, type]: schema_.node_types()) {
            node_type_ids_.emplace(type.name, type_id);
        }
    }

    PostgreSqlGraphStore::~PostgreSqlGraphStore() {
        std::scoped_lock lock(mutex_);
        if (connection_ != nullptr) {
            PQfinish(connection_);
            connection_ = nullptr;
        }
    }

    void PostgreSqlGraphStore::ensure_connection_locked() {
        if (connection_ != nullptr && PQstatus(connection_) == CONNECTION_OK) {
            return;
        }
        if (connection_ != nullptr) {
            PQfinish(connection_);
            connection_ = nullptr;
        }

        const auto [host, port] = parse_endpoint(connection_config_.endpoint);
        const std::string database =
                connection_config_.database_name.empty() ? "polygraph" : connection_config_.database_name;
        const std::string username =
                connection_config_.credentials.has_value() ? connection_config_.credentials->username : "polygraph";
        const std::string password =
                connection_config_.credentials.has_value() ? connection_config_.credentials->password : "polygraph";

        const std::array<const char *, 7> keywords{"host", "port", "dbname", "user", "password", "application_name",
                                                   nullptr};
        const std::array<const char *, 7> values{host.c_str(),     port.c_str(), database.c_str(), username.c_str(),
                                                 password.c_str(), "polygraph",  nullptr};
        connection_ = PQconnectdbParams(keywords.data(), values.data(), 0);
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string message = pq_error(connection_, "Could not connect to PostgreSQL");
            if (connection_ != nullptr) {
                PQfinish(connection_);
                connection_ = nullptr;
            }
            throw std::runtime_error(message);
        }

        execute_command_locked("SET statement_timeout = 0");
        execute_command_locked("SET lock_timeout = 0");
    }

    void PostgreSqlGraphStore::execute_command_locked(const std::string &sql) {
        PGresult *result = PQexec(connection_, sql.c_str());
        if (result == nullptr) {
            throw std::runtime_error(pq_error(connection_, "PostgreSQL command failed"));
        }
        const ExecStatusType status = PQresultStatus(result);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            const std::string message = PQresultErrorMessage(result);
            PQclear(result);
            throw std::runtime_error("PostgreSQL command failed: " + message);
        }
        PQclear(result);
    }

    void PostgreSqlGraphStore::begin_frontier_locked(const std::span<const NodeRef> frontier) {
        execute_command_locked("BEGIN");
        try {
            // The frontier is a short-lived hash-join input. Do not maintain or build a
            // B-tree here: for the default index-free PostgreSQL benchmark it adds work
            // every BFS level without helping the sequential edge scan. ANALYZE below
            // is enough for the planner to size the hash table accurately.
            execute_command_locked("CREATE TEMP TABLE polygraph_frontier(id BIGINT NOT NULL) ON COMMIT DROP");

            EVENT_SCOPE_INFO(copy_span, "postgresql.frontier.copy", {"frontier_size", frontier.size()},
                             {"copy_format", "binary"});
            PGresult *copy_result = PQexec(connection_, "COPY polygraph_frontier(id) FROM STDIN WITH (FORMAT binary)");
            if (copy_result == nullptr || PQresultStatus(copy_result) != PGRES_COPY_IN) {
                const std::string message =
                        copy_result != nullptr ? PQresultErrorMessage(copy_result) : PQerrorMessage(connection_);
                if (copy_result != nullptr) {
                    PQclear(copy_result);
                }
                throw std::runtime_error("PostgreSQL COPY frontier could not start: " + message);
            }
            PQclear(copy_result);

            // PostgreSQL binary COPY format:
            //   signature, flags, extension length,
            //   repeated [int16 field-count, int32 length, int64 value],
            //   int16 -1 trailer. Integer payloads use network byte order.
            std::string buffer;
            constexpr std::size_t flush_threshold = 1U << 20U;
            buffer.reserve(flush_threshold + 64U);
            buffer.append("PGCOPY\n\377\r\n\0", 11U);
            append_u32_be(buffer, 0U); // flags
            append_u32_be(buffer, 0U); // header extension length

            std::size_t copied_bytes = 0;
            const auto flush = [&]() {
                if (buffer.empty()) {
                    return;
                }
                if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                    throw std::runtime_error("PostgreSQL COPY buffer exceeds libpq size limit");
                }
                if (PQputCopyData(connection_, buffer.data(), static_cast<int>(buffer.size())) != 1) {
                    throw std::runtime_error(pq_error(connection_, "PostgreSQL COPY frontier write failed"));
                }
                copied_bytes += buffer.size();
                buffer.clear();
            };

            for (const NodeRef &node: frontier) {
                append_u16_be(buffer, 1U); // one column
                append_u32_be(buffer, 8U); // int8/BIGINT payload size
                append_u64_be(buffer, static_cast<std::uint64_t>(to_postgres_id(
                                              insertion::BackendEncodedId::canonical_id(node.global_id))));
                if (buffer.size() >= flush_threshold) {
                    flush();
                }
            }
            append_u16_be(buffer, std::numeric_limits<std::uint16_t>::max()); // int16 -1 trailer
            flush();

            if (PQputCopyEnd(connection_, nullptr) != 1) {
                throw std::runtime_error(pq_error(connection_, "PostgreSQL COPY frontier finish failed"));
            }
            while (PGresult *result = PQgetResult(connection_)) {
                const ExecStatusType status = PQresultStatus(result);
                if (status != PGRES_COMMAND_OK) {
                    const std::string message = PQresultErrorMessage(result);
                    PQclear(result);
                    throw std::runtime_error("PostgreSQL COPY frontier failed: " + message);
                }
                PQclear(result);
            }
            EVENT_SCOPE_INFO_FIELD(copy_span, "copy_bytes", copied_bytes);

            {
                EVENT_SCOPE_INFO(analyze_span, "postgresql.frontier.analyze", {"frontier_size", frontier.size()});
                execute_command_locked("ANALYZE polygraph_frontier");
            }
        } catch (...) {
            finish_frontier_locked(false);
            throw;
        }
    }

    void PostgreSqlGraphStore::finish_frontier_locked(const bool commit) noexcept {
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            return;
        }
        PGresult *result = PQexec(connection_, commit ? "COMMIT" : "ROLLBACK");
        if (result != nullptr) {
            PQclear(result);
        }
    }

    std::string PostgreSqlGraphStore::quote_literal(const std::string_view value) {
        std::string result;
        result.reserve(value.size() + 2U);
        result.push_back('\'');
        for (const char character: value) {
            if (character == '\'') {
                result.push_back('\'');
            }
            result.push_back(character);
        }
        result.push_back('\'');
        return result;
    }

    std::string PostgreSqlGraphStore::edge_filter_sql(const ExpandSpec &spec, const std::string_view alias) const {
        if (spec.allowed_edge_types.empty()) {
            return {};
        }
        std::string sql = " AND ";
        sql += alias;
        sql += ".edge_type IN (";
        bool first = true;
        for (const TypeId type_id: spec.allowed_edge_types) {
            if (!first) {
                sql += ',';
            }
            first = false;
            sql += quote_literal(schema_.edge_type(type_id).name);
        }
        sql += ')';
        return sql;
    }

    std::string PostgreSqlGraphStore::target_filter_sql(const ExpandSpec &spec, const std::string_view column) const {
        if (spec.allowed_target_node_types.empty()) {
            return {};
        }
        std::string sql = " AND ";
        sql += column;
        sql += " IN (";
        bool first = true;
        for (const TypeId type_id: spec.allowed_target_node_types) {
            if (!first) {
                sql += ',';
            }
            first = false;
            sql += quote_literal(schema_.node_type(type_id).name);
        }
        sql += ')';
        return sql;
    }

    std::string PostgreSqlGraphStore::build_candidate_query(const ExpandSpec &spec, const bool include_source) const {
        const auto select_outbound = [&]() {
            std::string query = "SELECT ";
            if (include_source) {
                query += "f.id AS source_id,";
            }
            query += "e.to_id AS neighbor_id,e.to_type AS neighbor_type "
                     "FROM polygraph_frontier f JOIN polygraph_edges e ON e.from_id=f.id WHERE TRUE";
            query += edge_filter_sql(spec, "e");
            query += target_filter_sql(spec, "e.to_type");
            return query;
        };
        const auto select_inbound = [&]() {
            std::string query = "SELECT ";
            if (include_source) {
                query += "f.id AS source_id,";
            }
            query += "e.from_id AS neighbor_id,e.from_type AS neighbor_type "
                     "FROM polygraph_frontier f JOIN polygraph_edges e ON e.to_id=f.id WHERE TRUE";
            query += edge_filter_sql(spec, "e");
            query += target_filter_sql(spec, "e.from_type");
            return query;
        };

        std::string query = select_outbound();
        if (spec.direction != Direction::Directed) {
            query += " UNION ALL ";
            query += select_inbound();
        }
        if (server_side_candidate_deduplication_) {
            const std::string columns =
                    include_source ? "source_id,neighbor_id,neighbor_type" : "neighbor_id,neighbor_type";
            query = "SELECT DISTINCT " + columns + " FROM (" + query + ") polygraph_candidates";
        }
        return query;
    }

    NodeRef PostgreSqlGraphStore::parse_neighbor(PGresult *result, const int id_column, const int type_column) const {
        const std::string_view id_text(PQgetvalue(result, 0, id_column));
        const std::string_view type_name(PQgetvalue(result, 0, type_column));
        const auto type = node_type_ids_.find(std::string(type_name));
        if (type == node_type_ids_.end()) {
            throw std::runtime_error("PostgreSQL returned unknown node type: " + std::string(type_name));
        }
        return NodeRef{.global_id = from_postgres_id(id_text), .type = type->second};
    }

    std::size_t PostgreSqlGraphStore::stream_query_locked(const std::string &sql,
                                                          const std::size_t candidate_batch_size,
                                                          const NeighborBatchConsumer &consumer) {
        if (PQsendQuery(connection_, sql.c_str()) != 1) {
            throw std::runtime_error(pq_error(connection_, "PostgreSQL query send failed"));
        }
        if (PQsetSingleRowMode(connection_) != 1) {
            throw std::runtime_error(pq_error(connection_, "PostgreSQL single-row mode could not be enabled"));
        }

        std::vector<NodeRef> candidates;
        candidates.reserve(candidate_batch_size);
        std::size_t candidate_count = 0;
        while (PGresult *result = PQgetResult(connection_)) {
            const ExecStatusType status = PQresultStatus(result);
            if (status == PGRES_SINGLE_TUPLE) {
                candidates.push_back(parse_neighbor(result, 0, 1));
                ++candidate_count;
                if (candidates.size() >= candidate_batch_size) {
                    consumer(std::span<const NodeRef>(candidates));
                    candidates.clear();
                }
            } else if (status != PGRES_TUPLES_OK) {
                const std::string message = PQresultErrorMessage(result);
                PQclear(result);
                throw std::runtime_error("PostgreSQL frontier join failed: " + message);
            }
            PQclear(result);
        }
        if (!candidates.empty()) {
            consumer(std::span<const NodeRef>(candidates));
        }
        return candidate_count;
    }

    void PostgreSqlGraphStore::stream_neighbor_candidates(const std::span<const NodeRef> frontier,
                                                          const ExpandSpec &spec, const int depth,
                                                          const BfsOptions &options,
                                                          const NeighborBatchConsumer &consumer) {
        if (frontier.empty() || !consumer) {
            return;
        }
        (void) depth;
        EVENT_SCOPE_INFO(total_span, "postgresql.stream_neighbor_candidates", {"depth", depth},
                         {"frontier_size", frontier.size()},
                         {"candidate_batch_size", options.streaming_candidate_batch_size});
        std::scoped_lock lock(mutex_);
        ensure_connection_locked();
        begin_frontier_locked(frontier);
        try {
            std::size_t candidate_count = 0;
            {
                EVENT_SCOPE_INFO(join_span, "postgresql.frontier.join", {"frontier_size", frontier.size()},
                                 {"server_side_deduplication", server_side_candidate_deduplication_});
                candidate_count = stream_query_locked(build_candidate_query(spec, false),
                                                      options.streaming_candidate_batch_size, consumer);
                EVENT_SCOPE_INFO_FIELD(join_span, "candidate_count", candidate_count);
            }
            EVENT_SCOPE_INFO_FIELD(total_span, "candidate_count", candidate_count);
            finish_frontier_locked(true);
        } catch (...) {
            finish_frontier_locked(false);
            throw;
        }
    }

    std::vector<MinimalAdjacencyRow>
    PostgreSqlGraphStore::get_minimal_neighbors_batch(const std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                      const int depth, const BfsOptions &options) {
        (void) depth;
        (void) options;
        if (frontier.empty()) {
            return {};
        }

        std::vector<MinimalAdjacencyRow> rows;
        rows.reserve(frontier.size());
        std::unordered_map<GlobalId, std::size_t> row_by_source;
        row_by_source.reserve(frontier.size());
        for (const NodeRef &source: frontier) {
            row_by_source.emplace(source.global_id, rows.size());
            rows.push_back(MinimalAdjacencyRow{.source = source, .neighbors = {}});
        }

        std::scoped_lock lock(mutex_);
        ensure_connection_locked();
        begin_frontier_locked(frontier);
        try {
            const std::string query = build_candidate_query(spec, true);
            if (PQsendQuery(connection_, query.c_str()) != 1 || PQsetSingleRowMode(connection_) != 1) {
                throw std::runtime_error(pq_error(connection_, "PostgreSQL adjacency query could not start"));
            }
            while (PGresult *result = PQgetResult(connection_)) {
                const ExecStatusType status = PQresultStatus(result);
                if (status == PGRES_SINGLE_TUPLE) {
                    const GlobalId source_id = from_postgres_id(PQgetvalue(result, 0, 0));
                    const auto row = row_by_source.find(source_id);
                    if (row != row_by_source.end()) {
                        rows[row->second].neighbors.push_back(parse_neighbor(result, 1, 2));
                    }
                } else if (status != PGRES_TUPLES_OK) {
                    const std::string message = PQresultErrorMessage(result);
                    PQclear(result);
                    throw std::runtime_error("PostgreSQL adjacency query failed: " + message);
                }
                PQclear(result);
            }
            finish_frontier_locked(true);
        } catch (...) {
            finish_frontier_locked(false);
            throw;
        }
        return rows;
    }

    std::vector<AdjacencyRow> PostgreSqlGraphStore::get_neighbors_batch(const std::span<const NodeRef> frontier,
                                                                        const ExpandSpec &spec, const int depth) {
        BfsOptions options;
        options.return_predecessors = true;
        auto minimal = get_minimal_neighbors_batch(frontier, spec, depth, options);
        std::vector<AdjacencyRow> rows;
        rows.reserve(minimal.size());
        for (auto &row: minimal) {
            AdjacencyRow rich{.source = row.source, .neighbors = {}};
            rich.neighbors.reserve(row.neighbors.size());
            for (const NodeRef &neighbor: row.neighbors) {
                rich.neighbors.push_back(NeighborRef{.vertex = neighbor, .via_edge = std::nullopt});
            }
            rows.push_back(std::move(rich));
        }
        return rows;
    }

} // namespace polygraph
