#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include "Internal.hpp"
#include "LdbcSpec.hpp"

namespace polygraph::ingestion::detail {
    namespace {
        class LdbcIds {
        public:
            Id node(const Record &r, Backend backend) {
                const auto text = r.key.substr(r.key.find(':') + 1);
                std::uint64_t raw = 0;
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), raw);
                if (error != std::errc{} || end != text.data() + text.size() || text.empty())
                    throw Error("LDBC ID must be a nonnegative uint64: " + r.key);
                const auto pair = std::pair(r.type, raw >> 31);
                auto it = prefixes_.find(pair);
                if (it == prefixes_.end()) {
                    if (prefixes_.size() >= 65536)
                        throw Error("LDBC block31 prefix dictionary exhausted (65536 entries)");
                    it = prefixes_.emplace(pair, prefixes_.size()).first;
                }
                return make_id(backend, (static_cast<Id>(it->second) << 31) | (raw & ((Id{1} << 31) - 1)));
            }
            Id edge(const Record &r, Backend backend) {
                const auto it = std::ranges::find_if(edge_specs, [&](const auto &s) { return s.type == r.type; });
                if (it == edge_specs.end())
                    throw Error("Unknown LDBC relation type: " + r.type);
                auto &sequence = sequences_[{r.type, backend}];
                if (sequence >= (Id{1} << 42))
                    throw Error("LDBC relationship sequence exhausted");
                return make_id(backend, (Id{1} << 47) | (static_cast<Id>(it - edge_specs.begin()) << 42) | sequence++);
            }
            Json metadata() const {
                Json entries = Json::array();
                for (const auto &[pair, code]: prefixes_)
                    entries[code] = {{"type", pair.first}, {"rawPrefix", pair.second}, {"code", code}};
                return entries;
            }

        private:
            std::map<std::pair<std::string, Id>, std::size_t> prefixes_;
            std::map<std::pair<std::string, Backend>, Id> sequences_;
        };
        void validate_record(const Record &r) {
            if (r.type.empty() || r.type.find('\0') != std::string::npos)
                throw Error("Empty/invalid entity type");
            if (r.type == "Proxy")
                throw Error("Proxy is reserved for generated cross-backend vertices");
            validate_properties(r.properties);
        }
    } // namespace
    Json normalize(const PrepareOptions &o, const Placement &placement, const fs::path &scratch) {
        const auto start = Clock::now();
        Database db(scratch / "graph.sqlite3", std::max(1U, o.memory_limit_mb / 4));
        db.exec(R"(
            CREATE TABLE identities(id INTEGER PRIMARY KEY);
            CREATE TABLE nodes(id INTEGER PRIMARY KEY,original INTEGER NOT NULL,key TEXT NOT NULL UNIQUE,type TEXT NOT NULL,owner TEXT NOT NULL,properties TEXT NOT NULL,residency INTEGER NOT NULL);
            CREATE TABLE edges(id INTEGER PRIMARY KEY,original INTEGER NOT NULL,type TEXT NOT NULL,owner TEXT NOT NULL,from_id INTEGER NOT NULL,to_id INTEGER NOT NULL,properties TEXT NOT NULL);
            CREATE TABLE replicas(backend TEXT NOT NULL,edge_id INTEGER NOT NULL,PRIMARY KEY(backend,edge_id)) WITHOUT ROWID;
            CREATE TABLE proxies(backend TEXT NOT NULL,node_id INTEGER NOT NULL,PRIMARY KEY(backend,node_id)) WITHOUT ROWID;
            CREATE TABLE partitions(key TEXT PRIMARY KEY,partition INTEGER NOT NULL) WITHOUT ROWID;
            CREATE INDEX nodes_owner ON nodes(owner,id);
        )");
        const bool partitioned = partition_metadata_enabled(o);
        const auto partition_map = load_partition_mapping(o, placement);
        auto partitioning = prepare_partition_assignments(o, db, scratch);
        auto adapter = make_adapter(o, scratch);
        const auto id_policy = input_adapters(o).get(o.adapter).id_policy;
        if (id_policy == InputIdPolicy::typed_external)
            db.exec("CREATE TABLE source_edges(key TEXT PRIMARY KEY,id INTEGER NOT NULL UNIQUE) WITHOUT ROWID");
        auto entity = db.statement("INSERT INTO identities VALUES(?)");
        const auto register_id = [&](Id id, const Record &record, bool edge) {
            entity.bind(1, signed_id(id));
            try {
                entity.run();
            } catch (const Error &e) {
                throw Error("GlobalId collision/error " + std::to_string(id) + " on " +
                            (edge ? "relationship " : "vertex ") + record.type + " " + record.key + ": " + e.what());
            }
        };
        auto node_insert = db.statement("INSERT INTO nodes VALUES(?,?,?,?,?,?,?)");
        auto edge_insert = db.statement("INSERT INTO edges VALUES(?,?,?,?,?,?,?)");
        auto lookup =
                db.statement(partitioned ? "SELECT n.id,n.type,n.owner,p.partition FROM nodes n JOIN partitions p "
                                           "ON p.key=n.key WHERE n.key=?"
                                         : "SELECT id,type,owner,-1 FROM nodes WHERE key=?");
        auto partition = db.statement("SELECT partition FROM partitions WHERE key=?");
        auto replica = db.statement("INSERT INTO replicas VALUES(?,?)");
        auto proxy = db.statement("INSERT OR IGNORE INTO proxies VALUES(?,?)");
        LdbcIds ids; // Explicit legacy compatibility policy only; never a generic fallback.
        auto source_edge = db.statement(
                id_policy == InputIdPolicy::typed_external ? "INSERT INTO source_edges VALUES(?,?)" : "SELECT 1");
        Id next_payload = 0;
        const auto generated_id = [&](Backend home) {
            if (next_payload >= (Id{1} << 48))
                throw Error("Typed external ID payload space exhausted (48 bits)");
            return make_id(home, next_payload++);
        };
        std::uint64_t nodes = 0, edges = 0, cut = 0;
        std::map<std::string, std::string> safe_types;
        std::size_t type_bytes = 0;
        const auto check_type = [&](const std::string &type) {
            const auto safe = sanitize_name(type);
            if (safe == "Proxy" || safe == "polygraph_bulk_import_lock")
                throw Error("Entity type aliases reserved Proxy collection");
            if (safe.size() > 200)
                throw Error("Sanitized type name exceeds the 200-byte artifact/collection limit");
            const auto [it, inserted] = safe_types.emplace(safe, type);
            if (inserted) {
                type_bytes += safe.size() + type.size() + 64;
                if (safe_types.size() > 4096 || type_bytes > 1024 * 1024)
                    throw Error("Type dictionary exceeds bounded 4096-type/1-MiB limit");
            }
            if (!inserted && it->second != type)
                throw Error("Entity types collide after label/collection sanitization: " + type + " and " + it->second);
        };
        const auto node_start = Clock::now();
        db.exec("BEGIN");
        adapter->nodes([&](Record r) {
            resolve_record_keys(r, id_policy, false);
            if (!r.id && id_policy == InputIdPolicy::canonical)
                throw Error("Adapter " + o.adapter + " requires canonical GlobalId on node " + r.key);
            validate_record(r);
            check_type(r.type);
            auto home = placement.route(r, false);
            if (partitioned) {
                if (r.properties.contains("partition_id"))
                    throw Error("Input property partition_id collides with generated partition metadata");
                partition.bind(1, r.key);
                if (!partition.step())
                    throw Error("Missing partition assignment for " + r.key);
                const auto assigned_partition = partition.integer(0);
                partition.reset();
                r.properties["partition_id"] = assigned_partition;
                if (!partition_map.empty()) {
                    const auto p = std::to_string(assigned_partition);
                    if (!partition_map.contains(p))
                        throw Error("Partition has no backend mapping: " + p);
                    const auto assigned = parse_backend(partition_map[p].get<std::string>());
                    if (r.id && assigned != home)
                        throw Error("Partition mapping cannot rewrite canonical IDs");
                    home = assigned;
                }
            }
            if (r.source && *r.source != home)
                throw Error("Record source disagrees with placement on node " + r.key);
            const auto id = r.id                                         ? *r.id
                            : id_policy == InputIdPolicy::typed_external ? generated_id(home)
                                                                         : ids.node(r, home);
            const auto canonical = canonical_id(id);
            register_id(canonical, r, false);
            node_insert.bind(1, signed_id(canonical));
            node_insert.bind(2, signed_id(id));
            node_insert.bind(3, r.key);
            node_insert.bind(4, r.type);
            node_insert.bind(5, backend_name(home));
            node_insert.bind(6, r.properties.dump());
            node_insert.bind(7, static_cast<std::int64_t>((id >> 48) ^ (canonical >> 48)));
            try {
                node_insert.run();
            } catch (const Error &e) {
                throw Error("Duplicate/colliding vertex source identity " + r.key + ": " + e.what());
            }
            if (++nodes % 50000 == 0)
                db.exec("COMMIT; BEGIN");
        });
        db.exec("COMMIT; BEGIN");
        const auto node_seconds = elapsed(node_start);
        const auto edge_start = Clock::now();
        adapter->relationships([&](Record r) {
            resolve_record_keys(r, id_policy, true);
            if (!r.id && id_policy == InputIdPolicy::canonical)
                throw Error("Adapter " + o.adapter + " requires canonical GlobalId on relationship " + r.type);
            validate_record(r);
            check_type(r.type);
            const auto home = placement.route(r, true);
            if (r.source && *r.source != home)
                throw Error("Record source disagrees with placement on relationship " + r.type);
            const auto id = r.id                                         ? *r.id
                            : id_policy == InputIdPolicy::typed_external ? generated_id(home)
                                                                         : ids.edge(r, home);
            const auto canonical = canonical_id(id);
            register_id(canonical, r, true);
            if (id_policy == InputIdPolicy::typed_external && r.identity) {
                source_edge.bind(1, r.key);
                source_edge.bind(2, signed_id(canonical));
                try {
                    source_edge.run();
                } catch (const Error &e) {
                    throw Error("Duplicate/colliding relationship source identity " + r.key + ": " + e.what());
                }
            }
            struct Endpoint {
                std::int64_t id;
                std::string home;
                std::int64_t partition;
            };
            const auto resolve = [&](const std::string &key) {
                lookup.bind(1, key);
                if (!lookup.step())
                    throw Error("Unresolved relationship endpoint " + key + " on " + r.type);
                Endpoint result{lookup.integer(0), lookup.text(2), lookup.integer(3)};
                lookup.reset();
                return result;
            };
            const auto from = resolve(r.from), to = resolve(r.to);
            if (partitioned) {
                for (const auto *key: {"from_partition_id", "to_partition_id", "is_partition_cut"})
                    if (r.properties.contains(key))
                        throw Error("Input property " + std::string(key) +
                                    " collides with generated partition metadata");
                r.properties["from_partition_id"] = from.partition;
                r.properties["to_partition_id"] = to.partition;
                r.properties["is_partition_cut"] = from.partition != to.partition;
            }
            edge_insert.bind(1, signed_id(canonical));
            edge_insert.bind(2, signed_id(id));
            edge_insert.bind(3, r.type);
            edge_insert.bind(4, backend_name(home));
            edge_insert.bind(5, from.id);
            edge_insert.bind(6, to.id);
            edge_insert.bind(7, r.properties.dump());
            edge_insert.run();
            const std::set<std::string> stores{backend_name(home), from.home, to.home};
            for (const auto &store: stores) {
                replica.bind(1, store);
                replica.bind(2, signed_id(canonical));
                replica.run();
                for (const auto &endpoint: {from, to})
                    if (endpoint.home != store) {
                        proxy.bind(1, store);
                        proxy.bind(2, endpoint.id);
                        proxy.run();
                    }
            }
            if (from.home != to.home)
                ++cut;
            if (++edges % 50000 == 0)
                db.exec("COMMIT; BEGIN");
        });
        db.exec("COMMIT");
        const auto edge_seconds = elapsed(edge_start);
        const auto metadata = adapter->metadata();
        if (id_policy == InputIdPolicy::canonical) {
            if (!metadata.contains("idRouting") || !metadata["idRouting"].is_array())
                throw Error("Canonical adapter " + o.adapter + " metadata requires idRouting array");
            std::set<Backend> configured;
            for (const auto &name: metadata["idRouting"]) {
                const auto backend = parse_backend(name.get<std::string>());
                placement.validate(backend);
                if (!configured.insert(backend).second)
                    throw Error("Duplicate idRouting entry");
            }
            if (configured != std::set<Backend>(placement.backends.begin(), placement.backends.end()))
                throw Error("Canonical adapter " + o.adapter + " idRouting differs from placement storageBackends");
            if (metadata.value("idRoutingBits", 16) != 16 || metadata.value("idPayloadBits", 48) != 48)
                throw Error("Unsupported canonical ID layout for adapter " + o.adapter +
                            "; expected 16 location / 48 payload bits");
        }
        finish_partition_metadata(o, db, scratch, partitioning);
        partitioning["backend_cross_edges"] = cut;
        return {{"counts", {{"nodes", nodes}, {"edges", edges}}},
                {"metadata", metadata},
                {"node_prefixes", ids.metadata()},
                {"timings",
                 {{"nodes_seconds", node_seconds},
                  {"edges_seconds", edge_seconds},
                  {"normalization_seconds", elapsed(start)}}},
                {"partitioning", partitioning}};
    }
} // namespace polygraph::ingestion::detail
