#include "data_access/adapters/PostgreSQL/RelationalMappingValidator.hpp"

#include <algorithm>
#include <memory>

namespace polygraph::postgres {
    namespace {
        using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
        [[noreturn]] void fail(const std::string &context, const std::string &message) {
            throw std::invalid_argument("PostgreSQL relational mapping " + context + ": " + message);
        }
        Result query(PGconn *connection, const char *sql, const std::vector<std::string> &parameters,
                     const std::string &context) {
            std::vector<const char *> values;
            for (const auto &p: parameters)
                values.push_back(p.c_str());
            Result result(PQexecParams(connection, sql, static_cast<int>(values.size()), nullptr, values.data(),
                                       nullptr, nullptr, 0),
                          PQclear);
            if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
                fail(context, "catalog query failed: " + std::string(result ? PQresultErrorMessage(result.get())
                                                                            : PQerrorMessage(connection)));
            return result;
        }
        bool boolean(const Result &r, int row, int column) {
            return std::string_view(PQgetvalue(r.get(), row, column)) == "t";
        }
        bool integer(Oid type) { return type == 20 || type == 21 || type == 23; }
        bool string(Oid type) { return type == 25 || type == 1043; }
        bool key_compatible(Oid type, KeyCodec codec) {
            switch (codec) {
                case KeyCodec::int64:
                    return integer(type);
                case KeyCodec::string:
                    return string(type);
                case KeyCodec::uuid:
                    return type == 2950;
            }
            return false;
        }
        bool property_compatible(Oid type, domain::PropertyType property) {
            switch (property) {
                case domain::PropertyType::string:
                    return string(type);
                case domain::PropertyType::integer:
                    return integer(type);
                case domain::PropertyType::boolean:
                    return type == 16;
                case domain::PropertyType::_float:
                    return type == 700 || type == 701;
                case domain::PropertyType::date:
                    return type == 1082;
                case domain::PropertyType::datetime:
                    return type == 1114 || type == 1184;
            }
            return false;
        }
        const RelationalColumnMetadata &column(PGconn *connection, const RelationalTableMetadata &t,
                                               const std::string &name, const std::string &context) {
            const auto found = t.columns.find(name);
            if (found == t.columns.end())
                fail(context, "missing column '" + name + "' in " + t.table.schema + "." + t.table.name);
            auto privilege =
                    query(connection,
                          "SELECT pg_catalog.has_column_privilege($1::pg_catalog.oid,$2::pg_catalog.text,'SELECT')",
                          {std::to_string(t.oid), name}, context);
            if (!boolean(privilege, 0, 0))
                fail(context, "SELECT privilege required for column '" + name + "'");
            return found->second;
        }
        RelationalTableMetadata inspect(PGconn *connection, const SqlTable &t, const std::string &context) {
            auto r = query(connection, R"(SELECT c.oid, c.relkind, pg_catalog.has_schema_privilege(n.oid,'USAGE'),
                    pg_catalog.row_security_active(c.oid),
                    EXISTS(SELECT 1 FROM pg_catalog.pg_inherits i WHERE i.inhparent OPERATOR(pg_catalog.=) c.oid)
                FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid OPERATOR(pg_catalog.=) c.relnamespace
                WHERE n.nspname OPERATOR(pg_catalog.=) $1 AND c.relname OPERATOR(pg_catalog.=) $2)",
                           {t.schema, t.name}, context);
            if (PQntuples(r.get()) != 1)
                fail(context, "missing table '" + t.schema + "." + t.name + "'");
            const auto kind = std::string_view(PQgetvalue(r.get(), 0, 1));
            if (kind != "r" && kind != "p")
                fail(context, "mapped object must be a regular or partitioned table");
            if (kind == "r" && boolean(r, 0, 4))
                fail(context, "traditional inheritance descendants do not share a global primary key");
            if (!boolean(r, 0, 2))
                fail(context, "USAGE privilege required on schema '" + t.schema + "'");
            if (boolean(r, 0, 3))
                fail(context, "active row-level security cannot guarantee a complete graph");
            RelationalTableMetadata result{
                    .table = t, .oid = static_cast<Oid>(std::stoul(PQgetvalue(r.get(), 0, 0))), .columns = {}};
            auto cols = query(connection, R"(SELECT attname, attnum, atttypid, atttypmod, NOT attnotnull,
                    attgenerated OPERATOR(pg_catalog.<>) '', atthasdef, attidentity
                FROM pg_catalog.pg_attribute WHERE attrelid OPERATOR(pg_catalog.=) $1::pg_catalog.oid AND attnum OPERATOR(pg_catalog.>) 0 AND NOT attisdropped)",
                              {std::to_string(result.oid)}, context);
            for (int row = 0; row < PQntuples(cols.get()); ++row) {
                result.columns.emplace(PQgetvalue(cols.get(), row, 0),
                                       RelationalColumnMetadata{
                                               .number = std::stoi(PQgetvalue(cols.get(), row, 1)),
                                               .type_oid = static_cast<Oid>(std::stoul(PQgetvalue(cols.get(), row, 2))),
                                               .type_modifier = std::stoi(PQgetvalue(cols.get(), row, 3)),
                                               .nullable = boolean(cols, row, 4),
                                               .generated = boolean(cols, row, 5),
                                               .has_default = boolean(cols, row, 6),
                                               .identity = PQgetvalue(cols.get(), row, 7)});
            }
            return result;
        }
        void primary_key(PGconn *c, const RelationalTableMetadata &t, const std::string &name, KeyCodec codec,
                         const std::string &context) {
            const auto &col = column(c, t, name, context);
            if (!key_compatible(col.type_oid, codec))
                fail(context, "primary key column '" + name + "' is incompatible with keyCodec");
            auto pk = query(c, R"(SELECT EXISTS(SELECT 1 FROM pg_catalog.pg_constraint
                WHERE conrelid OPERATOR(pg_catalog.=) $1::pg_catalog.oid AND contype OPERATOR(pg_catalog.=) 'p' AND convalidated AND conkey OPERATOR(pg_catalog.=) ARRAY[$2::pg_catalog.int2]))",
                            {std::to_string(t.oid), std::to_string(col.number)}, context);
            if (!boolean(pk, 0, 0))
                fail(context, "column '" + name + "' must be the complete scalar primary key");
        }
        void properties(PGconn *c, const RelationalTableMetadata &t, const std::vector<PropertyMapping> &props,
                        const std::string &context) {
            for (const auto &p: props) {
                const auto &col = column(c, t, p.column, context);
                if (!property_compatible(col.type_oid, p.type))
                    fail(context, "property '" + p.property + "' has unsupported/incompatible SQL type in column '" +
                                          p.column + "'");
            }
        }
        void endpoint(PGconn *c, const RelationalTableMetadata &t, const std::string &name, const NodeMapping &node,
                      const std::string &context) {
            if (!key_compatible(column(c, t, name, context).type_oid, node.key_codec))
                fail(context, "endpoint column '" + name + "' is incompatible with node '" + node.logical_type + "'");
        }
    } // namespace
    ValidatedRelationalMapping RelationalMappingValidator::validate(PGconn *connection,
                                                                    const RelationalMapping &mapping) const {
        if (!connection || PQstatus(connection) != CONNECTION_OK)
            fail("connection", "an open PostgreSQL connection is required");
        ValidatedRelationalMapping result;
        result.mapping_ = mapping;
        for (const auto &n: mapping.nodes()) {
            const auto context = "node '" + n.logical_type + "'";
            auto t = inspect(connection, n.table, context);
            primary_key(connection, t, n.primary_key, n.key_codec, context);
            properties(connection, t, n.properties, context);
            result.tables_.push_back(std::move(t));
        }
        const auto mapped_table = [&](TypeId type) -> const RelationalTableMetadata & {
            const auto &n = mapping.node(type);
            return *std::ranges::find(result.tables_, n.table, &RelationalTableMetadata::table);
        };
        for (const auto &r: mapping.relationships()) {
            const auto context = "relationship '" + r.logical_type + "'";
            const auto &from = mapping.node(r.from_type);
            const auto &to = mapping.node(r.to_type);
            if (r.kind == RelationshipKind::table) {
                auto t = inspect(connection, r.table, context);
                endpoint(connection, t, r.from_column, from, context);
                endpoint(connection, t, r.to_column, to, context);
                if (r.primary_key)
                    primary_key(connection, t, *r.primary_key, *r.key_codec, context);
                properties(connection, t, r.properties, context);
                result.tables_.push_back(std::move(t));
            } else {
                const auto &owner = r.owner == ForeignKeyOwner::from ? from : to;
                const auto &target = r.owner == ForeignKeyOwner::from ? to : from;
                const auto &ot = mapped_table(owner.type);
                const auto &tt = mapped_table(target.type);
                endpoint(connection, ot, r.column, target, context);
                const auto &fk = column(connection, ot, r.column, context);
                const auto &pk = column(connection, tt, target.primary_key, context);
                auto constraint = query(connection, R"(SELECT EXISTS(SELECT 1 FROM pg_catalog.pg_constraint
                    WHERE conrelid OPERATOR(pg_catalog.=) $1::pg_catalog.oid AND confrelid OPERATOR(pg_catalog.=) $2::pg_catalog.oid AND contype OPERATOR(pg_catalog.=) 'f' AND convalidated
                    AND conkey OPERATOR(pg_catalog.=) ARRAY[$3::pg_catalog.int2] AND confkey OPERATOR(pg_catalog.=) ARRAY[$4::pg_catalog.int2]))",
                                        {std::to_string(ot.oid), std::to_string(tt.oid), std::to_string(fk.number),
                                         std::to_string(pk.number)},
                                        context);
                if (!boolean(constraint, 0, 0))
                    fail(context,
                         "column '" + r.column + "' requires a validated foreign key to the mapped primary key");
            }
        }
        return result;
    }
} // namespace polygraph::postgres
