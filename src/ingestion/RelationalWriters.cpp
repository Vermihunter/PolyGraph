#include "Internal.hpp"
#if POLYGRAPH_INGESTION_HAS_LIBPQ
#include <algorithm>
#include <cmath>
#include <set>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"
#include "domain/graph/LogicalGraphSchemaJson.hpp"
namespace polygraph::ingestion::detail {
    Json relational_configuration(const PrepareOptions &o) {
        if (!o.relational_configuration.empty())
            return o.relational_configuration;
        if (o.relational_mapping.empty())
            return Json::object();
        const auto schema = read_json(o.relational_schema), mapping = read_json(o.relational_mapping);
        const auto logical = domain::logical_schema_from_json(schema);
        const auto parsed = postgres::parse_relational_mapping(mapping, *logical);
        return {{"schema", schema},
                {"mapping", mapping},
                {"mapping_sha256", postgres::relational_mapping_fingerprint(parsed)}};
    }
    namespace {
        Json cell(const Json &v, domain::PropertyType type, const std::string &context) {
            if (v.is_null())
                return nullptr;
            switch (type) {
                case domain::PropertyType::integer:
                    if (!v.is_number_integer() || (v.is_number_unsigned() && v.get<std::uint64_t>() > INT64_MAX))
                        throw Error(context + ": exact signed integer required");
                    return std::to_string(v.get<std::int64_t>());
                case domain::PropertyType::boolean:
                    if (v.is_boolean())
                        return v.get<bool>() ? "true" : "false";
                    break;
                case domain::PropertyType::_float:
                    if (v.is_number() && std::isfinite(v.get<double>()))
                        return Json(v.get<double>()).dump();
                    break;
                case domain::PropertyType::string:
                case domain::PropertyType::date:
                case domain::PropertyType::datetime:
                    if (v.is_string() && v.get_ref<const std::string &>().find('\0') == std::string::npos)
                        return v;
                    break;
            }
            throw Error(context + ": unsupported property value/type");
        }
        domain::SourceIdentity source(const std::string &encoded, bool edge) {
            const auto key = Json::parse(encoded);
            if (!key.is_array() || key.size() != 6 || key[0] != "polygraph.source.v1" ||
                key[2] != (edge ? "edge" : "vertex"))
                throw Error("Relational import requires typed source keys");
            return domain::source_identity_from_json(
                    {{"namespace", key[1]}, {"type", key[3]}, {"idType", key[4]}, {"id", key[5]}});
        }
        std::string node_key(const postgres::NodeMapping &node, const std::string &encoded) {
            const auto original = source(encoded, false);
            const auto identity = postgres::relational_source_identity(node, original.value);
            if (domain::encode_source_identity(identity) != encoded)
                throw Error("Relational source identity must use mapped namespace/type and canonical key codec");
            return node.key_codec == postgres::KeyCodec::int64 ? identity.value.dump()
                                                               : identity.value.get<std::string>();
        }
        void column(Json &columns, Json &values, const std::string &name, Json value) {
            const auto found = std::find(columns.begin(), columns.end(), Json(name));
            if (found != columns.end()) {
                if (values[static_cast<std::size_t>(found - columns.begin())] != value)
                    throw Error("Property/identity conflicts with native key/endpoint column: " + name);
            } else {
                columns.push_back(name);
                values.push_back(std::move(value));
            }
        }
        void properties(Json &columns, Json &values, const Json &props,
                        const std::vector<postgres::PropertyMapping> &mappings, const std::string &type) {
            for (const auto &p: mappings)
                if (props.contains(p.property)) {
                    column(columns, values, p.column, cell(props.at(p.property), p.type, type + "." + p.property));
                }
        }
    } // namespace
    Json generate_relational_postgres(Database &db, const PrepareOptions &o, const fs::path &directory,
                                      const fs::path &root) {
        const auto config = relational_configuration(o);
        auto logical = domain::logical_schema_from_json(config.at("schema"));
        const auto mapping = postgres::parse_relational_mapping(config.at("mapping"), *logical);
        bool foreign_keys = false;
        for (const auto &r: mapping.relationships())
            foreign_keys = foreign_keys || r.kind == postgres::RelationshipKind::foreign_key;
        if (foreign_keys) {
            // TEMP belongs to this read-only SQLite connection, never the immutable index.
            db.exec("CREATE TEMP TABLE relational_fk(owner_id INTEGER,type TEXT,target_id INTEGER,target_key "
                    "TEXT,PRIMARY KEY(owner_id,type)) WITHOUT ROWID; BEGIN");
            auto candidates = db.statement(
                    "SELECT e.type,f.id,f.key,f.type,f.owner,t.id,t.key,t.type,t.owner FROM replicas r "
                    "JOIN edges e ON e.id=r.edge_id JOIN nodes f ON f.id=e.from_id JOIN nodes t ON t.id=e.to_id "
                    "WHERE r.backend='PostgreSQL' ORDER BY e.id");
            auto existing = db.statement("SELECT target_id FROM relational_fk WHERE owner_id=? AND type=?");
            auto insert = db.statement("INSERT INTO relational_fk VALUES(?,?,?,?)");
            while (candidates.step()) {
                for (const auto &r: mapping.relationships()) {
                    if (r.kind != postgres::RelationshipKind::foreign_key || r.logical_type != candidates.text(0))
                        continue;
                    if (candidates.text(3) != mapping.node(r.from_type).logical_type ||
                        candidates.text(7) != mapping.node(r.to_type).logical_type)
                        throw Error("FK endpoint logical type differs from mapping: " + r.logical_type);
                    const int owner = r.owner == postgres::ForeignKeyOwner::from ? 1 : 5;
                    const int target = owner == 1 ? 5 : 1;
                    if (candidates.text(owner + 3) != "PostgreSQL")
                        throw Error("FK owner must be a newly imported PostgreSQL node, not an existing reference row");
                    existing.bind(1, candidates.integer(owner));
                    existing.bind(2, r.logical_type);
                    if (existing.step()) {
                        if (existing.integer(0) != candidates.integer(target))
                            throw Error("Conflicting FK targets for owner/column: " + r.logical_type + "." + r.column);
                        existing.reset();
                        continue;
                    }
                    existing.reset();
                    insert.bind(1, candidates.integer(owner));
                    insert.bind(2, r.logical_type);
                    insert.bind(3, candidates.integer(target));
                    insert.bind(4, node_key(mapping.node(owner == 1 ? r.to_type : r.from_type),
                                            candidates.text(target + 1)));
                    insert.run();
                }
            }
            db.exec("COMMIT");
        }
        Json artifacts = Json::array();
        std::unique_ptr<AtomicOutput> output;
        fs::path path;
        std::uint64_t bytes = 0, count = 0, total = 0, edge_count = 0, proxy_count = 0;
        unsigned sequence = 0;
        std::string kind, collection;
        const auto close = [&] {
            if (!output)
                return;
            output->commit();
            output.reset();
            artifacts.push_back({{"path", fs::relative(path, root).generic_string()},
                                 {"kind", kind},
                                 {"collection", collection},
                                 {"records", count},
                                 {"bytes", fs::file_size(path)},
                                 {"sha256", sha256_file(path)}});
        };
        const auto write = [&](const Json &row, const std::string &row_kind, const std::string &row_collection = "") {
            const auto line = row.dump() + "\n";
            if (line.size() > static_cast<std::uint64_t>(o.max_record_mb) * 1024 * 1024)
                throw Error("Relational artifact row exceeds max-record-mb");
            if (output && (row_kind != kind || collection != row_collection ||
                           bytes + line.size() > static_cast<std::uint64_t>(o.chunk_size_mb) * 1024 * 1024))
                close();
            kind = row_kind;
            collection = row_collection;
            if (!output) {
                path = directory / (kind + "-" + std::to_string(sequence++) + ".jsonl");
                output = std::make_unique<AtomicOutput>(path);
                bytes = count = 0;
            }
            output->write(line);
            bytes += line.size();
            ++count;
        };
        const auto assignment = [&](std::int64_t id, const std::string &type) {
            auto q = db.statement("SELECT target_id,target_key FROM relational_fk WHERE owner_id=? AND type=?");
            q.bind(1, id);
            q.bind(2, type);
            if (q.step())
                return Json{{"id", q.integer(0)}, {"key", q.text(1)}};
            return Json{{"id", nullptr}, {"key", nullptr}};
        };
        auto rows = db.statement(
                std::string("SELECT id,key,type,properties FROM nodes WHERE owner='PostgreSQL' ORDER BY ") +
                (foreign_keys ? "type,id" : "id"));
        while (rows.step()) {
            const auto type = rows.text(2);
            const auto &node = mapping.node(logical->node_type_id(type));
            const auto pk = node_key(node, rows.text(1));
            Json columns = Json::array({node.primary_key}), values = Json::array({pk});
            properties(columns, values, Json::parse(rows.text(3)), node.properties, type);
            if (foreign_keys)
                for (const auto &r: mapping.relationships())
                    if (r.kind == postgres::RelationshipKind::foreign_key &&
                        (r.owner == postgres::ForeignKeyOwner::from ? r.from_type : r.to_type) == node.type)
                        column(columns, values, r.column, assignment(rows.integer(0), r.logical_type).at("key"));
            write({{"id", rows.integer(0)}, {"type", type}, {"columns", columns}, {"values", values}},
                  "relational_nodes", foreign_keys ? type : "");
            ++total;
        }
        auto edges = db.statement("SELECT e.id,e.type,e.properties,f.id,f.key,f.type,t.id,t.key,t.type,s.key "
                                  "FROM replicas r JOIN edges e ON e.id=r.edge_id JOIN nodes f ON f.id=e.from_id "
                                  "JOIN nodes t ON t.id=e.to_id LEFT JOIN source_edges s ON s.id=e.id "
                                  "WHERE r.backend='PostgreSQL' ORDER BY e.id");
        while (edges.step()) {
            const auto type = edges.text(1);
            const postgres::RelationshipMapping *relation = nullptr;
            for (const auto &r: mapping.relationships())
                if (r.logical_type == type)
                    relation = &r;
            if (!relation)
                throw Error("Relationship mapping required for " + type);
            const auto &from = mapping.node(relation->from_type), &to = mapping.node(relation->to_type);
            if (edges.text(5) != from.logical_type || edges.text(8) != to.logical_type)
                throw Error("Relationship endpoint logical type differs from mapping: " + type);
            if (relation->kind == postgres::RelationshipKind::foreign_key) {
                ++edge_count;
                continue;
            }
            Json columns = Json::array(), values = Json::array();
            if (relation->primary_key) {
                if (edges.text(9).empty())
                    throw Error("Mapped relationship PK requires an explicit typed edge identity");
                const auto identity = source(edges.text(9), true);
                const postgres::NodeMapping key_mapping{.type = relation->type,
                                                        .logical_type = type,
                                                        .table = relation->table,
                                                        .primary_key = *relation->primary_key,
                                                        .name_space = identity.name_space,
                                                        .key_codec = *relation->key_codec,
                                                        .properties = {}};
                const auto normalized = postgres::relational_source_identity(key_mapping, identity.value);
                if (domain::encode_source_identity(normalized, true) != edges.text(9))
                    throw Error("Relationship PK must use its canonical native key codec");
                columns.push_back(*relation->primary_key);
                values.push_back(*relation->key_codec == postgres::KeyCodec::int64
                                         ? normalized.value.dump()
                                         : normalized.value.get<std::string>());
            }
            column(columns, values, relation->from_column, node_key(from, edges.text(4)));
            column(columns, values, relation->to_column, node_key(to, edges.text(7)));
            properties(columns, values, Json::parse(edges.text(2)), relation->properties, type);
            write({{"entity", "relationship"},
                   {"id", edges.integer(0)},
                   {"type", type},
                   {"from_id", edges.integer(3)},
                   {"to_id", edges.integer(6)},
                   {"columns", columns},
                   {"values", values}},
                  "relational_relationships");
            ++edge_count;
        }
        auto proxies = db.statement("SELECT n.id,n.key,n.type,n.owner FROM proxies p JOIN nodes n ON n.id=p.node_id "
                                    "WHERE p.backend='PostgreSQL' ORDER BY n.id");
        while (proxies.step()) {
            const auto type = proxies.text(2);
            const auto &node = mapping.node(logical->node_type_id(type));
            write({{"entity", "reference"},
                   {"id", proxies.integer(0)},
                   {"owner", proxies.text(3)},
                   {"type", type},
                   {"columns", Json::array({node.primary_key})},
                   {"values", Json::array({node_key(node, proxies.text(1))})}},
                  "relational_references");
            ++proxy_count;
        }
        if (foreign_keys) {
            auto owners = db.statement("SELECT id,key,type FROM nodes WHERE owner='PostgreSQL' ORDER BY type,id");
            while (owners.step()) {
                const auto &n = mapping.node(logical->node_type_id(owners.text(2)));
                for (const auto &r: mapping.relationships()) {
                    if (r.kind != postgres::RelationshipKind::foreign_key ||
                        (r.owner == postgres::ForeignKeyOwner::from ? r.from_type : r.to_type) != n.type)
                        continue;
                    const auto target = assignment(owners.integer(0), r.logical_type);
                    Json columns = Json::array({n.primary_key}), values = Json::array({node_key(n, owners.text(1))});
                    column(columns, values, r.column, target.at("key"));
                    write({{"entity", "foreignKey"},
                           {"id", owners.integer(0)},
                           {"type", n.logical_type},
                           {"foreign_key", r.logical_type},
                           {"target_id", target.at("id")},
                           {"columns", columns},
                           {"values", values}},
                          "relational_foreign_keys");
                }
            }
        }
        close();
        return {{"artifacts", artifacts},
                {"counts", {{"nodes", total}, {"proxies", proxy_count}, {"edges", edge_count}}},
                {"importer", "libpq-relational-copy"},
                {"relational", config},
                {"state", "prepared"}};
    }
} // namespace polygraph::ingestion::detail
#else
namespace polygraph::ingestion::detail {
    Json relational_configuration(const PrepareOptions &o) {
        if (!o.relational_mapping.empty() || !o.relational_configuration.empty())
            throw Error("Relational ingestion requires native libpq support");
        return Json::object();
    }
    Json generate_relational_postgres(Database &, const PrepareOptions &, const fs::path &, const fs::path &) {
        throw Error("Relational ingestion requires native libpq support");
    }
} // namespace polygraph::ingestion::detail
#endif
