#include "data_access/adapters/PostgreSQL/PostgresRelationalGraphStore.hpp"
#include "RelationalRelationshipReadInternal.hpp"
#include "insertion/BackendEncodedUuid.hpp"

#include <algorithm>
#include <bit>
#include <sqlite3.h>
#include <unordered_map>

namespace polygraph {
    namespace {
        const std::string &required(const std::optional<std::string> &value, const char *name) {
            if (!value || value->empty())
                throw std::invalid_argument(std::string("PostgreSQL ") + name + " is required");
            return *value;
        }
        class CandidateSet {
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insert_{nullptr, sqlite3_finalize};

        public:
            CandidateSet() {
                sqlite3 *raw = nullptr;
                const auto status = sqlite3_open("", &raw);
                db_.reset(raw);
                if (status != SQLITE_OK)
                    throw std::runtime_error("Cannot open temporary candidate deduplication storage");
                if (sqlite3_exec(db_.get(),
                                 "PRAGMA cache_size=-4096; PRAGMA mmap_size=0; PRAGMA temp_store=FILE;"
                                 "CREATE TABLE seen(source INTEGER,target INTEGER,PRIMARY KEY(source,target)) WITHOUT "
                                 "ROWID; BEGIN",
                                 nullptr, nullptr, nullptr) != SQLITE_OK)
                    throw std::runtime_error("Cannot initialize candidate deduplication storage");
                sqlite3_stmt *statement = nullptr;
                if (sqlite3_prepare_v2(db_.get(), "INSERT OR IGNORE INTO seen VALUES(?,?)", -1, &statement, nullptr) !=
                    SQLITE_OK)
                    throw std::runtime_error("Cannot prepare candidate deduplication");
                insert_.reset(statement);
            }
            bool add(GlobalId source, GlobalId target) {
                if (sqlite3_bind_int64(insert_.get(), 1, std::bit_cast<std::int64_t>(source)) != SQLITE_OK ||
                    sqlite3_bind_int64(insert_.get(), 2, std::bit_cast<std::int64_t>(target)) != SQLITE_OK ||
                    sqlite3_step(insert_.get()) != SQLITE_DONE)
                    throw std::runtime_error("Candidate deduplication failed");
                const bool added = sqlite3_changes(db_.get()) != 0;
                sqlite3_reset(insert_.get());
                sqlite3_clear_bindings(insert_.get());
                return added;
            }
        };
        GlobalId canonical(GlobalId id) { return insertion::BackendEncodedId::canonical_id(id); }
    } // namespace
    PostgresRelationalGraphStore::PostgresRelationalGraphStore(BackendDescriptor descriptor,
                                                               BackendConnectionConfig connection,
                                                               const domain::LogicalGraphSchema &schema,
                                                               bool deduplicate) :
        descriptor_(std::move(descriptor)), config_(std::move(connection)),
        mapping_(postgres::load_relational_mapping(required(config_.relational_mapping, "relationalMapping"), schema)),
        identities_(required(config_.relational_identity, "relationalIdentity"), mapping_), deduplicate_(deduplicate) {
        for (const auto &[id, n]: schema.node_types()) {
            (void) n;
            node_types_.insert(id);
        }
        for (const auto &[id, e]: schema.edge_types()) {
            (void) e;
            edge_types_.insert(id);
        }
        descriptor_.capabilities.supports_batch_neighbors = true;
        descriptor_.capabilities.supports_native_bfs = false;
    }
    PostgresRelationalGraphStore::Connection PostgresRelationalGraphStore::acquire() {
        Connection connection{nullptr, PQfinish};
        {
            std::scoped_lock guard(idle_mutex_);
            connection = std::move(idle_);
        }
        if (connection && PQstatus(connection.get()) == CONNECTION_OK)
            return connection;
        connection.reset();
        const auto uri = config_.endpoint.starts_with("postgresql://") || config_.endpoint.starts_with("postgres://")
                                 ? config_.endpoint
                                 : "postgresql://" + config_.endpoint;
        std::vector<const char *> keys{"dbname"}, values{uri.c_str()};
        if (!config_.database_name.empty()) {
            keys.push_back("dbname");
            values.push_back(config_.database_name.c_str());
        }
        if (config_.credentials) {
            keys.push_back("user");
            values.push_back(config_.credentials->username.c_str());
            keys.push_back("password");
            values.push_back(config_.credentials->password.c_str());
        }
        keys.push_back("connect_timeout");
        values.push_back("15");
        keys.push_back("application_name");
        values.push_back("polygraph-relational");
        keys.push_back(nullptr);
        values.push_back(nullptr);
        connection.reset(PQconnectdbParams(keys.data(), values.data(), 1));
        if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
            throw std::runtime_error("Cannot connect to PostgreSQL relational backend");
        return connection;
    }
    void PostgresRelationalGraphStore::release(Connection connection) noexcept {
        if (!connection || PQstatus(connection.get()) != CONNECTION_OK ||
            PQtransactionStatus(connection.get()) != PQTRANS_IDLE)
            return;
        {
            std::scoped_lock guard(idle_mutex_);
            if (!idle_)
                idle_ = std::move(connection);
        }
        // Any surplus connection is closed here, outside the idle-slot lock.
    }
    void PostgresRelationalGraphStore::expand(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                              const BfsOptions &options,
                                              const postgres::RelationalRelationshipConsumer &consume) {
        if (!options.neighbor_batch_size || !options.streaming_candidate_batch_size)
            throw std::invalid_argument("PostgreSQL neighbor/candidate batch sizes must be positive");
        for (const auto type: spec.allowed_edge_types)
            if (!edge_types_.contains(type))
                throw std::invalid_argument("Unknown logical edge filter");
        for (const auto type: spec.allowed_target_node_types)
            if (!node_types_.contains(type))
                throw std::invalid_argument("Unknown logical node filter");
        if (spec.direction != Direction::Directed && spec.direction != Direction::Undirected &&
            spec.direction != Direction::Both)
            throw std::invalid_argument("Invalid expansion direction");
        std::vector<NodeRef> sources;
        std::unordered_map<GlobalId, std::size_t> positions;
        for (const auto &n: frontier) {
            const auto id = canonical(n.global_id);
            const auto [it, added] = positions.emplace(id, sources.size());
            if (added)
                sources.push_back({id, n.type});
            else {
                auto &prior = sources[it->second];
                if (prior.type != unresolved_type_id && n.type != unresolved_type_id && prior.type != n.type)
                    throw std::invalid_argument("Conflicting frontier node types");
                if (prior.type == unresolved_type_id)
                    prior.type = n.type;
            }
        }
        Lease lease(*this);
        const auto chunk = std::min(options.neighbor_batch_size, postgres::RelationalIdentityResolver::max_batch_size);
        const postgres::RelationalRelationshipLookup lookup{
                .relationship_types = spec.allowed_edge_types,
                .target_types = spec.allowed_target_node_types,
                .direction = spec.direction == Direction::Directed ? postgres::RelationshipLookupDirection::outgoing
                                                                   : postgres::RelationshipLookupDirection::both,
                .batch_size = std::min<std::size_t>(options.streaming_candidate_batch_size, 1024)};
        for (std::size_t begin = 0; begin < sources.size(); begin += chunk)
            postgres::detail::stream_relationships(
                    lease.connection.get(), std::span(sources).subspan(begin, std::min(chunk, sources.size() - begin)),
                    lookup, consume, mapping_, identities_, std::nullopt, false, false);
    }
    std::vector<MinimalAdjacencyRow>
    PostgresRelationalGraphStore::get_minimal_neighbors_batch(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                              int depth, const BfsOptions &options) {
        (void) depth;
        if (frontier.empty())
            return {};
        std::vector<MinimalAdjacencyRow> result;
        std::unordered_map<GlobalId, std::vector<std::size_t>> positions;
        for (const auto &n: frontier) {
            positions[canonical(n.global_id)].push_back(result.size());
            result.push_back({n, {}});
        }
        auto seen = deduplicate_ ? std::make_unique<CandidateSet>() : nullptr;
        expand(frontier, spec, options, [&](auto rows) {
            for (const auto &row: rows) {
                const auto target = row.reverse ? row.from.node : row.to.node;
                if (seen && !seen->add(row.origin.global_id, target.global_id))
                    continue;
                for (const auto position: positions.at(row.origin.global_id))
                    result[position].neighbors.push_back(target);
            }
        });
        return result;
    }
    std::vector<AdjacencyRow> PostgresRelationalGraphStore::get_neighbors_batch(std::span<const NodeRef> frontier,
                                                                                const ExpandSpec &spec, int depth) {
        auto minimal = get_minimal_neighbors_batch(frontier, spec, depth, {});
        std::vector<AdjacencyRow> result;
        for (auto &row: minimal) {
            AdjacencyRow rich{row.source, {}};
            for (const auto &n: row.neighbors)
                rich.neighbors.push_back({n, std::nullopt});
            result.push_back(std::move(rich));
        }
        return result;
    }
    void PostgresRelationalGraphStore::stream_neighbor_candidates(std::span<const NodeRef> frontier,
                                                                  const ExpandSpec &spec, int depth,
                                                                  const BfsOptions &options,
                                                                  const NeighborBatchConsumer &consume) {
        (void) depth;
        if (frontier.empty() || !consume)
            return;
        if (!options.streaming_candidate_batch_size)
            throw std::invalid_argument("Candidate batch size must be positive");
        const auto limit = std::min<std::size_t>(options.streaming_candidate_batch_size, 1024);
        std::vector<NodeRef> batch;
        batch.reserve(limit);
        auto seen = deduplicate_ ? std::make_unique<CandidateSet>() : nullptr;
        expand(frontier, spec, options, [&](auto rows) {
            for (const auto &row: rows) {
                const auto target = row.reverse ? row.from.node : row.to.node;
                if (seen && !seen->add(0, target.global_id))
                    continue;
                batch.push_back(target);
                if (batch.size() == limit) {
                    consume(batch);
                    batch.clear();
                }
            }
        });
        if (!batch.empty())
            consume(batch);
    }
} // namespace polygraph
