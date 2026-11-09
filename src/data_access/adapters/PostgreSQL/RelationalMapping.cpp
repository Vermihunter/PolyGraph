#include "data_access/adapters/PostgreSQL/RelationalMapping.hpp"

#include <algorithm>
#include <fstream>
#include <set>

namespace polygraph::postgres {
    namespace {
        [[noreturn]] void fail(const std::string &context, const std::string &message) {
            throw std::invalid_argument("PostgreSQL relational mapping " + context + ": " + message);
        }
        void fields(const json &j, std::initializer_list<std::string_view> allowed, const std::string &context) {
            if (!j.is_object())
                fail(context, "expected object");
            for (const auto &[key, value]: j.items()) {
                (void) value;
                if (std::ranges::find(allowed, key) == allowed.end())
                    fail(context, "unknown field '" + key + "'");
            }
        }
        std::string text(const json &j, const char *field, const std::string &context, bool identifier = false) {
            const auto it = j.find(field);
            if (it == j.end() || !it->is_string())
                fail(context, std::string(field) + " must be a string (scalar keys only)");
            auto value = it->get<std::string>();
            if (value.empty() || value.find('\0') != std::string::npos || (identifier && value.size() > 63))
                fail(context,
                     std::string(field) + " must be nonempty, NUL-free" + (identifier ? " and at most 63 bytes" : ""));
            return value;
        }
        KeyCodec codec(const json &j, const std::string &context) {
            const auto name = text(j, "keyCodec", context);
            if (name == "int64")
                return KeyCodec::int64;
            if (name == "string")
                return KeyCodec::string;
            if (name == "uuid")
                return KeyCodec::uuid;
            fail(context, "keyCodec must be int64, string or uuid");
        }
        SqlTable table(const json &j, const std::string &context) {
            return {text(j, "schema", context, true), text(j, "table", context, true)};
        }
        std::vector<PropertyMapping> properties(const json &j, const std::vector<domain::Property> &logical,
                                                const std::string &context) {
            std::vector<PropertyMapping> result;
            if (!j.contains("properties"))
                return result;
            const auto &p = j.at("properties");
            if (!p.is_object())
                fail(context, "properties must be an object");
            std::set<std::string> columns;
            for (const auto &[name, value]: p.items()) {
                (void) value;
                const auto found = std::ranges::find(logical, name, &domain::Property::name);
                if (found == logical.end())
                    fail(context, "unknown logical property '" + name + "'");
                auto column = text(p, name.c_str(), context + ".properties", true);
                if (!columns.insert(column).second)
                    fail(context, "duplicate property column '" + column + "'");
                result.push_back({name, std::move(column), found->type});
            }
            return result;
        }
    } // namespace
    const NodeMapping &RelationalMapping::node(TypeId type) const {
        const auto found = std::ranges::find(nodes_, type, &NodeMapping::type);
        if (found == nodes_.end())
            fail("endpoint", "no node mapping for logical type ID " + std::to_string(type));
        return *found;
    }
    RelationalMapping parse_relational_mapping(const json &j, const domain::LogicalGraphSchema &schema) {
        try {
            fields(j, {"format", "version", "nodes", "relationships"}, "root");
            if (text(j, "format", "root") != "polygraph.postgresql-relational")
                fail("root", "unsupported format");
            if (!j.contains("version") || !j["version"].is_number_integer() || j["version"] != 1)
                fail("root", "unsupported version; expected integer 1");
            if (!j.contains("nodes") || !j["nodes"].is_array() || j["nodes"].empty())
                fail("nodes", "expected nonempty array");
            if (!j.contains("relationships") || !j["relationships"].is_array())
                fail("relationships", "expected array");
            RelationalMapping result;
            std::set<TypeId> node_types, edge_types;
            std::set<std::pair<std::string, std::string>> physical_tables;
            for (const auto &n: j["nodes"]) {
                const auto name = text(n, "type", "node");
                const auto context = "node '" + name + "'";
                fields(n, {"type", "schema", "table", "primaryKey", "namespace", "keyCodec", "properties"}, context);
                const auto type = schema.node_type_id(name);
                if (!node_types.insert(type).second)
                    fail(context, "duplicate logical node mapping");
                auto t = table(n, context);
                if (!physical_tables.emplace(t.schema, t.name).second)
                    fail(context, "ambiguous table mapping");
                result.nodes_.push_back({type, name, std::move(t), text(n, "primaryKey", context, true),
                                         text(n, "namespace", context), codec(n, context),
                                         properties(n, schema.node_type(type).properties, context)});
            }
            std::set<std::pair<TypeId, std::string>> fk_columns;
            for (const auto &e: j["relationships"]) {
                const auto name = text(e, "type", "relationship");
                const auto context = "relationship '" + name + "'";
                const auto type = schema.edge_type_id(name);
                const auto &logical = schema.edge_type(type);
                (void) result.node(logical.from);
                (void) result.node(logical.to);
                if (!edge_types.insert(type).second)
                    fail(context, "duplicate logical relationship mapping");
                const auto kind = text(e, "kind", context);
                RelationshipMapping r{.type = type,
                                      .logical_type = name,
                                      .from_type = logical.from,
                                      .to_type = logical.to,
                                      .kind = RelationshipKind::table,
                                      .table = {},
                                      .from_column = {},
                                      .to_column = {},
                                      .primary_key = {},
                                      .key_codec = {},
                                      .properties = {},
                                      .owner = ForeignKeyOwner::from,
                                      .column = {}};
                if (kind == "table") {
                    fields(e,
                           {"type", "kind", "schema", "table", "fromColumn", "toColumn", "primaryKey", "keyCodec",
                            "properties"},
                           context);
                    r.table = table(e, context);
                    if (!physical_tables.emplace(r.table.schema, r.table.name).second)
                        fail(context, "ambiguous table mapping");
                    r.from_column = text(e, "fromColumn", context, true);
                    r.to_column = text(e, "toColumn", context, true);
                    if (r.from_column == r.to_column)
                        fail(context, "endpoint columns must differ");
                    if (e.contains("primaryKey") != e.contains("keyCodec"))
                        fail(context, "primaryKey and keyCodec must appear together");
                    if (e.contains("primaryKey")) {
                        r.primary_key = text(e, "primaryKey", context, true);
                        r.key_codec = codec(e, context);
                    }
                    r.properties = properties(e, logical.properties, context);
                } else if (kind == "foreignKey") {
                    fields(e, {"type", "kind", "owner", "column"}, context);
                    r.kind = RelationshipKind::foreign_key;
                    const auto owner = text(e, "owner", context);
                    if (owner != "from" && owner != "to")
                        fail(context, "owner must be from or to");
                    r.owner = owner == "from" ? ForeignKeyOwner::from : ForeignKeyOwner::to;
                    r.column = text(e, "column", context, true);
                    if (!fk_columns.emplace(r.owner == ForeignKeyOwner::from ? r.from_type : r.to_type, r.column)
                                 .second)
                        fail(context, "duplicate foreign-key column mapping");
                } else
                    fail(context, "kind must be table or foreignKey");
                result.relationships_.push_back(std::move(r));
            }
            return result;
        } catch (const std::invalid_argument &) {
            throw;
        } catch (const std::exception &e) {
            fail("configuration", e.what());
        }
    }
    RelationalMapping load_relational_mapping(const std::filesystem::path &path,
                                              const domain::LogicalGraphSchema &schema) {
        try {
            std::ifstream file(path);
            if (!file)
                fail(path.string(), "cannot open file");
            return parse_relational_mapping(json::parse(file), schema);
        } catch (const std::exception &e) {
            throw std::invalid_argument("Mapping file '" + path.string() + "': " + e.what());
        }
    }
    std::optional<std::filesystem::path> relational_mapping_path(const json &config,
                                                                 const std::filesystem::path &containing_file) {
        if (!config.contains("relationalMapping"))
            return std::nullopt;
        const std::filesystem::path path{text(config, "relationalMapping", "configuration")};
        if (path.is_absolute())
            return path.lexically_normal();
        return (std::filesystem::absolute(containing_file).parent_path() / path).lexically_normal();
    }
    std::string quote_identifier(std::string_view identifier) {
        if (identifier.empty() || identifier.find('\0') != std::string_view::npos || identifier.size() > 63)
            fail("identifier", "expected nonempty NUL-free name of at most 63 bytes");
        std::string result{"\""};
        for (const auto c: identifier) {
            result += c;
            if (c == '"')
                result += '"';
        }
        return result + '"';
    }
} // namespace polygraph::postgres
