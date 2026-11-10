#include "Internal.hpp"
#if POLYGRAPH_INGESTION_HAS_LIBPQ
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
    } // namespace
    Json generate_relational_postgres(Database &db, const PrepareOptions &o, const fs::path &directory,
                                      const fs::path &root) {
        const auto config = relational_configuration(o);
        auto logical = domain::logical_schema_from_json(config.at("schema"));
        const auto mapping = postgres::parse_relational_mapping(config.at("mapping"), *logical);
        auto edges = db.statement("SELECT count(*) FROM replicas WHERE backend='PostgreSQL'");
        if (!edges.step())
            throw Error("Cannot count relational edges");
        if (edges.integer(0) != 0)
            throw Error("Relational relationship ingestion requires #19/#20; refusing node-only import with edges");
        auto proxies = db.statement("SELECT count(*) FROM proxies WHERE backend='PostgreSQL'");
        if (!proxies.step())
            throw Error("Cannot count relational proxies");
        if (proxies.integer(0) != 0)
            throw Error("Relational node import cannot manufacture application proxy rows");
        Json artifacts = Json::array();
        std::unique_ptr<AtomicOutput> output;
        fs::path path;
        std::uint64_t bytes = 0, count = 0, total = 0;
        unsigned sequence = 0;
        const auto close = [&] {
            if (!output)
                return;
            output->commit();
            output.reset();
            artifacts.push_back({{"path", fs::relative(path, root).generic_string()},
                                 {"kind", "relational_nodes"},
                                 {"collection", ""},
                                 {"records", count},
                                 {"bytes", fs::file_size(path)},
                                 {"sha256", sha256_file(path)}});
        };
        auto rows = db.statement("SELECT id,key,type,properties FROM nodes WHERE owner='PostgreSQL' ORDER BY id");
        while (rows.step()) {
            const auto type = rows.text(2);
            const auto &node = mapping.node(logical->node_type_id(type));
            const auto key = Json::parse(rows.text(1));
            if (!key.is_array() || key.size() != 6 || key[0] != "polygraph.source.v1" || key[2] != "vertex")
                throw Error("Relational nodes require typed source keys");
            const auto original = domain::source_identity_from_json(
                    {{"namespace", key[1]}, {"type", key[3]}, {"idType", key[4]}, {"id", key[5]}});
            const auto identity = postgres::relational_source_identity(node, original.value);
            if (domain::encode_source_identity(identity) != rows.text(1))
                throw Error("Relational source identity must use mapped namespace/type and canonical key codec");
            const auto pk = node.key_codec == postgres::KeyCodec::int64 ? identity.value.dump()
                                                                        : identity.value.get<std::string>();
            Json columns = Json::array({node.primary_key}), values = Json::array({pk});
            const auto props = Json::parse(rows.text(3));
            for (const auto &p: node.properties)
                if (props.contains(p.property)) {
                    auto value = cell(props.at(p.property), p.type, type + "." + p.property);
                    if (p.column == node.primary_key) {
                        if (value != pk)
                            throw Error("Property conflicts with native primary key");
                        continue;
                    }
                    columns.push_back(p.column);
                    values.push_back(std::move(value));
                }
            const auto line =
                    Json{{"id", rows.integer(0)}, {"type", type}, {"columns", columns}, {"values", values}}.dump() +
                    "\n";
            if (line.size() > static_cast<std::uint64_t>(o.max_record_mb) * 1024 * 1024)
                throw Error("Relational artifact row exceeds max-record-mb");
            if (output && bytes + line.size() > static_cast<std::uint64_t>(o.chunk_size_mb) * 1024 * 1024)
                close();
            if (!output) {
                path = directory / ("relational-nodes-" + std::to_string(sequence++) + ".jsonl");
                output = std::make_unique<AtomicOutput>(path);
                bytes = count = 0;
            }
            output->write(line);
            bytes += line.size();
            ++count;
            ++total;
        }
        close();
        return {{"artifacts", artifacts},
                {"counts", {{"nodes", total}, {"proxies", 0}, {"edges", 0}}},
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
