#include <algorithm>
#include <limits>
#include <map>
#include "Internal.hpp"

namespace polygraph::ingestion::detail {
    namespace {
        constexpr char array_delimiter = '\x1f';
        struct Counts {
            std::uint64_t nodes = 0, proxies = 0, edges = 0;
        };
        class Chunks {
        public:
            Chunks(const fs::path &directory, const fs::path &root, std::string stem, std::string extension,
                   Json &artifacts, unsigned chunk_mb, std::string kind, std::string collection = {},
                   std::string header = {}) :
                directory_(directory), root_(root), stem_(std::move(stem)), extension_(std::move(extension)),
                artifacts_(artifacts), limit_(static_cast<std::uint64_t>(chunk_mb) * 1024 * 1024),
                kind_(std::move(kind)), collection_(std::move(collection)), header_(std::move(header)) {}
            void row(const std::string &line) {
                if (output_ && bytes_ + line.size() > limit_ && count_ > 0)
                    close();
                if (!output_) {
                    path_ = directory_ / (stem_ + "-" + std::to_string(sequence_++) + extension_);
                    output_ = std::make_unique<AtomicOutput>(path_);
                    if (!header_.empty())
                        output_->write(header_);
                    bytes_ = header_.size();
                    count_ = 0;
                }
                output_->write(line);
                bytes_ += line.size();
                ++count_;
            }
            void close() {
                if (!output_)
                    return;
                output_->commit();
                output_.reset();
                artifacts_.push_back({{"path", fs::relative(path_, root_).generic_string()},
                                      {"kind", kind_},
                                      {"collection", collection_},
                                      {"records", count_},
                                      {"bytes", fs::file_size(path_)},
                                      {"sha256", sha256_file(path_)}});
            }

        private:
            fs::path directory_, root_, path_;
            std::string stem_, extension_;
            Json &artifacts_;
            std::uint64_t limit_, bytes_ = 0, count_ = 0, sequence_ = 0;
            std::string kind_, collection_, header_;
            std::unique_ptr<AtomicOutput> output_;
        };
        std::string property_type(const Json &value) {
            if (value.is_null())
                return "";
            if (value.is_boolean())
                return "boolean";
            if (value.is_number_integer()) {
                if (value.is_number_unsigned() &&
                    value.get<Id>() > static_cast<Id>(std::numeric_limits<std::int64_t>::max()))
                    throw Error("Neo4j property integer exceeds signed INT64; use a string representation");
                return "long";
            }
            if (value.is_number_float())
                return "double";
            if (value.is_string())
                return "string";
            if (value.is_array()) {
                std::string element;
                for (const auto &item: value) {
                    const auto type = property_type(item);
                    if (type.empty() || type.ends_with("[]"))
                        throw Error("Neo4j cannot store null/nested array elements");
                    if (!element.empty() && element != type)
                        throw Error("Neo4j requires homogeneous property arrays");
                    element = type;
                }
                return element.empty() ? "unknown[]" : element + "[]";
            }
            throw Error("Neo4j cannot store object properties; use a JSON string explicitly");
        }
        using Schema = std::map<std::string, std::string>;
        bool reserved(const std::string &key, bool edge) {
            return key == "uuid" || key == "localId" || key == "type" || key == "source" ||
                   (!edge && key == "is_proxy");
        }
        Schema infer_schema(Database &db, const std::string &sql, const std::string &type, bool edge) {
            auto rows = db.statement(sql);
            rows.bind(1, type);
            Schema schema;
            std::size_t schema_bytes = 0;
            while (rows.step()) {
                const auto properties = Json::parse(rows.text(0));
                for (const auto &[key, value]: properties.items()) {
                    if (reserved(key, edge) || value.is_null())
                        continue;
                    if (key.empty() || key.find_first_of(":{}\r\n\x1f") != std::string::npos)
                        throw Error("Neo4j CSV cannot represent property header: " + key);
                    const auto kind = property_type(value);
                    auto [it, inserted] = schema.emplace(key, kind);
                    if (inserted) {
                        schema_bytes += key.size() + kind.size() + 64;
                        if (schema.size() > 1024 || schema_bytes > 65536)
                            throw Error("Neo4j schema exceeds bounded 1024-column/64-KiB limit for type " + type);
                    }
                    if (!inserted && it->second != kind) {
                        if (it->second == "unknown[]" && kind.ends_with("[]"))
                            it->second = kind;
                        else if (kind != "unknown[]" || !it->second.ends_with("[]"))
                            it->second = "dynamic";
                    }
                }
            }
            for (auto &[key, kind]: schema) {
                (void) key;
                if (kind == "unknown[]")
                    kind = "string[]";
            }
            return schema;
        }
        std::string neo_value(const Json &value) {
            if (value.is_null())
                return "";
            if (value.is_string())
                return value.get<std::string>();
            if (value.is_array()) {
                std::string result;
                for (std::size_t i = 0; i < value.size(); ++i) {
                    if (i)
                        result += array_delimiter;
                    const auto item = neo_value(value[i]);
                    if (item.find(array_delimiter) != std::string::npos || item.empty())
                        return "";
                    result += item;
                }
                return result;
            }
            return value.dump();
        }
        void array_fixups(Chunks &fixups, const Schema &schema, const Json &properties, std::int64_t id, bool edge) {
            Json patch = Json::object();
            for (const auto &[key, type]: schema) {
                if (!properties.contains(key) || properties[key].is_null()) {
                    patch[key] = nullptr; // Remove importer-created empty string/array for absent/null values.
                    continue;
                }
                const auto &value = properties[key];
                bool needs_patch = type == "dynamic";
                if (value.is_array()) {
                    needs_patch = needs_patch || value.empty();
                    for (const auto &item: value)
                        if (item.is_string()) {
                            const auto &text = item.get_ref<const std::string &>();
                            needs_patch =
                                    needs_patch || text.empty() || text.find(array_delimiter) != std::string::npos;
                        }
                }
                if (needs_patch)
                    patch[key] = value;
            }
            if (!patch.empty())
                fixups.row(Json{{"id", id}, {"edge", edge}, {"properties", patch}}.dump() + "\n");
        }
        void append_properties(std::vector<std::string> &row, const Schema &schema, const Json &properties) {
            for (const auto &[key, type]: schema) {
                (void) type;
                row.push_back(type == "dynamic" ? "" : properties.contains(key) ? neo_value(properties[key]) : "");
            }
        }
        void append_headers(std::vector<std::string> &row, const Schema &schema) {
            for (const auto &[key, type]: schema)
                row.push_back(key + ":" + (type == "dynamic" ? "string" : type));
        }
        std::string signed_text(std::int64_t id) { return std::to_string(id); }
        std::string unsigned_text(std::int64_t id) { return std::to_string(std::bit_cast<Id>(id)); }
        std::string payload_text(std::int64_t id) { return std::to_string(local_id(std::bit_cast<Id>(id))); }
        Json proxy_properties(const std::string &type, const std::string &source, std::int64_t partition) {
            Json properties = {{"is_proxy", true}, {"remote-type", type}, {"remote-source", source}};
            if (partition >= 0)
                properties["partition_id"] = partition;
            return properties;
        }
        std::string proxy_query(const std::string &backend, bool partition_metadata) {
            return "SELECT n.id,n.type,n.owner," + std::string(partition_metadata ? "coalesce(q.partition,-1)" : "-1") +
                   " FROM proxies p JOIN nodes n ON n.id=p.node_id " +
                   (partition_metadata ? "LEFT JOIN partitions q ON q.key=n.key " : "") + "WHERE p.backend='" +
                   backend + "' ORDER BY n.id";
        }
        void postgres(Database &db, const PrepareOptions &o, const fs::path &directory, const fs::path &root,
                      Json &artifacts, Counts &counts, bool partition_metadata) {
            Chunks nodes(directory, root, "nodes", ".csv", artifacts, o.chunk_size_mb, "nodes");
            auto n =
                    db.statement("SELECT id,type,properties,residency FROM nodes WHERE owner='PostgreSQL' ORDER BY id");
            while (n.step()) {
                auto props = Json::parse(n.text(2));
                props.erase("is_proxy");
                nodes.row(csv_row({signed_text(n.integer(0)), n.text(1), "PostgreSQL", "false",
                                   std::to_string(n.integer(3)), props.dump()}));
                ++counts.nodes;
            }
            auto p = db.statement(proxy_query("PostgreSQL", partition_metadata));
            while (p.step()) {
                if (partition_metadata && p.integer(3) < 0)
                    throw Error("Proxy has no partition assignment");
                auto props = proxy_properties(p.text(1), p.text(2), p.integer(3));
                props.erase("is_proxy");
                nodes.row(csv_row({signed_text(p.integer(0)), p.text(1), "PostgreSQL", "true", "0", props.dump()}));
                ++counts.proxies;
            }
            nodes.close();
            Chunks edges(directory, root, "edges", ".csv", artifacts, o.chunk_size_mb, "edges");
            auto e = db.statement("SELECT e.id,e.type,e.owner,e.from_id,f.type,e.to_id,t.type,e.properties FROM "
                                  "replicas r JOIN edges e ON e.id=r.edge_id JOIN nodes f ON f.id=e.from_id JOIN nodes "
                                  "t ON t.id=e.to_id WHERE r.backend='PostgreSQL' ORDER BY e.id");
            while (e.step()) {
                edges.row(csv_row({signed_text(e.integer(0)), e.text(1), e.text(2), signed_text(e.integer(3)),
                                   e.text(4), signed_text(e.integer(5)), e.text(6), e.text(7)}));
                ++counts.edges;
            }
            edges.close();
        }
        void arango(Database &db, const PrepareOptions &o, const fs::path &directory, const fs::path &root,
                    Json &artifacts, Counts &counts, bool partition_metadata) {
            auto types = db.statement("SELECT DISTINCT type FROM nodes WHERE owner='ArangoDB' ORDER BY type");
            while (types.step()) {
                const auto type = types.text(0), collection = sanitize_name(type);
                Chunks chunks(directory, root, "nodes-" + collection, ".jsonl", artifacts, o.chunk_size_mb, "nodes",
                              collection);
                auto rows = db.statement(
                        "SELECT id,properties,residency FROM nodes WHERE owner='ArangoDB' AND type=? ORDER BY id");
                rows.bind(1, type);
                while (rows.step()) {
                    auto doc = Json::parse(rows.text(1));
                    if (rows.integer(2) != 0)
                        doc["cache_residency"] = rows.integer(2);
                    doc["_key"] = unsigned_text(rows.integer(0));
                    doc["uuid"] = rows.integer(0);
                    doc["type"] = type;
                    doc["source"] = "ArangoDB";
                    chunks.row(doc.dump() + "\n");
                    ++counts.nodes;
                }
                chunks.close();
            }
            Chunks proxies(directory, root, "proxies", ".jsonl", artifacts, o.chunk_size_mb, "proxies", "Proxy");
            auto p = db.statement(proxy_query("ArangoDB", partition_metadata));
            while (p.step()) {
                if (partition_metadata && p.integer(3) < 0)
                    throw Error("Proxy has no partition assignment");
                auto doc = proxy_properties(p.text(1), p.text(2), p.integer(3));
                doc["_key"] = unsigned_text(p.integer(0));
                doc["uuid"] = p.integer(0);
                doc["type"] = "Proxy";
                doc["source"] = "ArangoDB";
                proxies.row(doc.dump() + "\n");
                ++counts.proxies;
            }
            proxies.close();
            auto relations = db.statement("SELECT DISTINCT e.type FROM replicas r JOIN edges e ON e.id=r.edge_id WHERE "
                                          "r.backend='ArangoDB' ORDER BY e.type");
            while (relations.step()) {
                const auto type = relations.text(0), collection = sanitize_name(type);
                for (const auto &a: artifacts)
                    if (a.at("collection") == collection && a.at("kind") != "edges")
                        throw Error("Arango vertex/relationship collection name collision: " + collection);
                Chunks chunks(directory, root, "edges-" + collection, ".jsonl", artifacts, o.chunk_size_mb, "edges",
                              collection);
                auto rows = db.statement(
                        "SELECT e.id,e.owner,e.from_id,f.type,f.owner,e.to_id,t.type,t.owner,e.properties FROM "
                        "replicas r JOIN edges e ON e.id=r.edge_id JOIN nodes f ON f.id=e.from_id JOIN nodes t ON "
                        "t.id=e.to_id WHERE r.backend='ArangoDB' AND e.type=? ORDER BY e.id");
                rows.bind(1, type);
                while (rows.step()) {
                    auto doc = Json::parse(rows.text(8));
                    doc["_key"] = unsigned_text(rows.integer(0));
                    doc["uuid"] = rows.integer(0);
                    doc["type"] = type;
                    doc["source"] = rows.text(1);
                    doc["_from"] = (rows.text(4) == "ArangoDB" ? sanitize_name(rows.text(3)) : "Proxy") + "/" +
                                   unsigned_text(rows.integer(2));
                    doc["_to"] = (rows.text(7) == "ArangoDB" ? sanitize_name(rows.text(6)) : "Proxy") + "/" +
                                 unsigned_text(rows.integer(5));
                    chunks.row(doc.dump() + "\n");
                    ++counts.edges;
                }
                chunks.close();
            }
        }
        void neo4j(Database &db, const PrepareOptions &o, const fs::path &directory, const fs::path &root,
                   Json &artifacts, Counts &counts, bool partition_metadata) {
            Chunks fixups(directory, root, "property-fixups", ".jsonl", artifacts, o.chunk_size_mb, "fixups");
            auto types = db.statement("SELECT DISTINCT type FROM nodes WHERE owner='Neo4J' ORDER BY type");
            while (types.step()) {
                const auto type = types.text(0);
                auto schema =
                        infer_schema(db, "SELECT properties FROM nodes WHERE owner='Neo4J' AND type=?", type, false);
                auto residency =
                        db.statement("SELECT count(*) FROM nodes WHERE owner='Neo4J' AND type=? AND residency!=0");
                residency.bind(1, type);
                (void) residency.step();
                const bool has_residency = residency.integer(0) != 0;
                if (has_residency)
                    schema["cache_residency"] = "long";
                std::vector<std::string> header{":ID",           "localId:long",     "type:string",
                                                "source:string", "is_proxy:boolean", ":LABEL"};
                append_headers(header, schema);
                Chunks chunks(directory, root, "nodes-" + sanitize_name(type), ".csv", artifacts, o.chunk_size_mb,
                              "nodes", sanitize_name(type), csv_row(header));
                auto rows = db.statement(
                        "SELECT id,properties,residency FROM nodes WHERE owner='Neo4J' AND type=? ORDER BY id");
                rows.bind(1, type);
                while (rows.step()) {
                    std::vector<std::string> row{unsigned_text(rows.integer(0)),
                                                 payload_text(rows.integer(0)),
                                                 type,
                                                 "Neo4J",
                                                 "false",
                                                 sanitize_name(type) + array_delimiter + "POLYGRAPHVertex" +
                                                         array_delimiter + "__GraphNode"};
                    auto properties = Json::parse(rows.text(1));
                    if (has_residency)
                        properties["cache_residency"] = rows.integer(2);
                    array_fixups(fixups, schema, properties, rows.integer(0), false);
                    append_properties(row, schema, properties);
                    chunks.row(csv_row(row));
                    ++counts.nodes;
                }
                chunks.close();
            }
            std::vector<std::string> proxy_header{":ID",
                                                  "uuid:long",
                                                  "type:string",
                                                  "source:string",
                                                  "is_proxy:boolean",
                                                  "remote-type:string",
                                                  "remote-source:string",
                                                  ":LABEL"};
            if (partition_metadata)
                proxy_header.push_back("partition_id:long");
            Chunks proxies(directory, root, "proxies", ".csv", artifacts, o.chunk_size_mb, "proxies", "Proxy",
                           csv_row(proxy_header));
            auto p = db.statement(proxy_query("Neo4J", partition_metadata));
            while (p.step()) {
                std::vector<std::string> row{unsigned_text(p.integer(0)),
                                             signed_text(p.integer(0)),
                                             "Proxy",
                                             "Neo4J",
                                             "true",
                                             p.text(1),
                                             p.text(2),
                                             std::string("Proxy") + array_delimiter + "POLYGRAPHVertex" +
                                                     array_delimiter + "__GraphNode"};
                if (partition_metadata) {
                    if (p.integer(3) < 0)
                        throw Error("Proxy has no partition assignment");
                    row.push_back(std::to_string(p.integer(3)));
                }
                proxies.row(csv_row(row));
                ++counts.proxies;
            }
            proxies.close();
            auto relations = db.statement("SELECT DISTINCT e.type FROM replicas r JOIN edges e ON e.id=r.edge_id WHERE "
                                          "r.backend='Neo4J' ORDER BY e.type");
            while (relations.step()) {
                const auto type = relations.text(0);
                const auto schema = infer_schema(db,
                                                 "SELECT e.properties FROM replicas r JOIN edges e ON e.id=r.edge_id "
                                                 "WHERE r.backend='Neo4J' AND e.type=?",
                                                 type, true);
                std::vector<std::string> header{":START_ID", ":END_ID",     ":TYPE",        "localId:long",
                                                "uuid:long", "type:string", "source:string"};
                append_headers(header, schema);
                Chunks chunks(directory, root, "edges-" + sanitize_name(type), ".csv", artifacts, o.chunk_size_mb,
                              "edges", sanitize_name(type), csv_row(header));
                auto rows =
                        db.statement("SELECT e.id,e.owner,e.from_id,e.to_id,e.properties FROM replicas r JOIN edges e "
                                     "ON e.id=r.edge_id WHERE r.backend='Neo4J' AND e.type=? ORDER BY e.id");
                rows.bind(1, type);
                while (rows.step()) {
                    const bool native = rows.text(1) == "Neo4J";
                    std::vector<std::string> row{unsigned_text(rows.integer(2)),
                                                 unsigned_text(rows.integer(3)),
                                                 sanitize_name(type),
                                                 native ? payload_text(rows.integer(0)) : "",
                                                 native ? "" : signed_text(rows.integer(0)),
                                                 type,
                                                 rows.text(1)};
                    const auto properties = Json::parse(rows.text(4));
                    array_fixups(fixups, schema, properties, rows.integer(0), true);
                    append_properties(row, schema, properties);
                    chunks.row(csv_row(row));
                    ++counts.edges;
                }
                chunks.close();
            }
            fixups.close();
        }
    } // namespace
    Json generate_backend(Backend backend, const PrepareOptions &o, const fs::path &scratch, const fs::path &generation,
                          const fs::path &root, bool partition_metadata) {
        const auto started = Clock::now();
        const auto name = backend_name(backend);
        const auto directory = generation / name;
        fs::create_directories(directory);
        Database db(scratch / "graph.sqlite3", std::max(1U, o.memory_limit_mb / (4 * std::min(o.threads, 3U))), true);
        if (backend == Backend::postgresql && (!o.relational_mapping.empty() || !o.relational_configuration.empty()))
            return generate_relational_postgres(db, o, directory, root);
        Counts counts;
        Json artifacts = Json::array();
        switch (backend) {
            case Backend::postgresql:
                postgres(db, o, directory, root, artifacts, counts, partition_metadata);
                break;
            case Backend::arango:
                arango(db, o, directory, root, artifacts, counts, partition_metadata);
                break;
            case Backend::neo4j:
                neo4j(db, o, directory, root, artifacts, counts, partition_metadata);
                break;
        }
        std::uint64_t bytes = 0;
        for (const auto &artifact: artifacts)
            bytes += artifact["bytes"].get<std::uint64_t>();
        const auto seconds = elapsed(started);
        return {{"artifacts", artifacts},
                {"counts", {{"nodes", counts.nodes}, {"proxies", counts.proxies}, {"edges", counts.edges}}},
                {"bytes", bytes},
                {"seconds", seconds},
                {"bytes_per_second", seconds > 0 ? static_cast<double>(bytes) / seconds : 0},
                {"importer", backend == Backend::neo4j    ? "neo4j-admin-full"
                             : backend == Backend::arango ? "http-import"
                                                          : "libpq-copy"},
                {"state", "prepared"}};
    }
} // namespace polygraph::ingestion::detail
