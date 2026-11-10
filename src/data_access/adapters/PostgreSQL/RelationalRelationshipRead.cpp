#include "RelationalFrontier.hpp"
#include "RelationalReadSnapshot.hpp"
#include "RelationalRelationshipReadInternal.hpp"
#include "RelationalSqlValue.hpp"
#include "data_access/adapters/PostgreSQL/RelationalMappingValidator.hpp"

#include <algorithm>
#include <unordered_set>

namespace polygraph::postgres {
    namespace {
        bool allows(const std::vector<TypeId> &types, TypeId type) {
            return types.empty() || std::ranges::find(types, type) != types.end();
        }
        using detail::frontier_sql;
        using detail::physical_table;
        using detail::pk_expression;
        std::string table_query(const RelationshipMapping &r, const NodeMapping &from, const NodeMapping &to,
                                bool reverse, const ValidatedRelationalMapping &metadata) {
            const auto column = [](const char *alias, const std::string &name) {
                return std::string(alias) + "." + quote_identifier(name);
            };
            const auto &source = reverse ? to : from;
            std::string result = frontier_sql(source.key_codec) + "SELECT frontier.ordinal," +
                                 column("e", r.from_column) + "," + column("e", r.to_column) + "," +
                                 column("f", from.primary_key) + "," + column("t", to.primary_key) + "," +
                                 (r.primary_key ? column("e", *r.primary_key) : "NULL::pg_catalog.text");
            for (const auto &property: r.properties)
                result += "," + column("e", property.column);
            result += " FROM " + physical_table(r.table) + " e LEFT JOIN " + physical_table(from.table) + " f ON " +
                      pk_expression(from, "f", metadata) + " OPERATOR(pg_catalog.=) " + column("e", r.from_column) +
                      " LEFT JOIN " + physical_table(to.table) + " t ON " + pk_expression(to, "t", metadata) +
                      " OPERATOR(pg_catalog.=) " + column("e", r.to_column) + " JOIN frontier ON " +
                      pk_expression(source, reverse ? "t" : "f", metadata) + " OPERATOR(pg_catalog.=) frontier.key";
            return result;
        }

        std::string foreign_key_query(const RelationshipMapping &r, const NodeMapping &from, const NodeMapping &to,
                                      bool reverse, const ValidatedRelationalMapping &metadata) {
            const bool owner_from = r.owner == ForeignKeyOwner::from;
            const auto &owner = owner_from ? from : to;
            const auto &reference = owner_from ? to : from;
            const auto &source = reverse ? to : from;
            const char *owner_alias = owner_from ? "f" : "t";
            const char *reference_alias = owner_from ? "t" : "f";
            const auto column = [](const char *alias, const std::string &name) {
                return std::string(alias) + "." + quote_identifier(name);
            };
            const auto foreign_key = column(owner_alias, r.column);
            const auto owner_key = column(owner_alias, owner.primary_key);
            return frontier_sql(source.key_codec) + "SELECT frontier.ordinal," +
                   (owner_from ? owner_key : foreign_key) + "," + (owner_from ? foreign_key : owner_key) + ",f." +
                   quote_identifier(from.primary_key) + ",t." + quote_identifier(to.primary_key) +
                   ",NULL::pg_catalog.text FROM " + physical_table(owner.table) + " " + owner_alias + " LEFT JOIN " +
                   physical_table(reference.table) + " " + reference_alias + " ON " +
                   pk_expression(reference, reference_alias, metadata) + " OPERATOR(pg_catalog.=) " + foreign_key +
                   " JOIN frontier ON " + pk_expression(source, reverse ? "t" : "f", metadata) +
                   " OPERATOR(pg_catalog.=) frontier.key WHERE " + foreign_key + " IS NOT NULL";
        }
    } // namespace
    void detail::stream_relationships(PGconn *connection, std::span<const NodeRef> frontier,
                                      const RelationalRelationshipLookup &lookup,
                                      const RelationalRelationshipConsumer &consume, const RelationalMapping &mapping_,
                                      const RelationalIdentityResolver &identities_, RelationshipKind selected_kind) {
        if (!connection || PQstatus(connection) != CONNECTION_OK || PQtransactionStatus(connection) != PQTRANS_IDLE)
            throw std::invalid_argument("Relationship lookup requires an idle PostgreSQL connection");
        if (!consume || lookup.batch_size == 0 || lookup.batch_size > 1024 ||
            frontier.size() > RelationalIdentityResolver::max_batch_size)
            throw std::invalid_argument(
                    "Relationship lookup requires a consumer, frontier <=4096 and batch size 1..1024");
        if (lookup.direction != RelationshipLookupDirection::outgoing &&
            lookup.direction != RelationshipLookupDirection::incoming &&
            lookup.direction != RelationshipLookupDirection::both)
            throw std::invalid_argument("Invalid relationship lookup direction");
        for (const auto type: lookup.relationship_types) {
            const auto it = std::ranges::find(mapping_.relationships(), type, &RelationshipMapping::type);
            if (it == mapping_.relationships().end() || it->kind != selected_kind)
                throw std::invalid_argument("Unknown or unsupported relationship type for this reader");
        }
        for (const auto type: lookup.target_types)
            (void) mapping_.node(type);
        if (frontier.empty())
            return;
        const auto origins = identities_.reverse(frontier);
        detail::RelationalReadSnapshot snapshot(connection, mapping_);
        (void) detail::pg_query(connection, "SET LOCAL DateStyle='ISO,YMD'");
        (void) detail::pg_query(connection, "SET LOCAL TimeZone='UTC'");
        (void) detail::pg_query(connection, "SET LOCAL extra_float_digits=3");
        const auto metadata = RelationalMappingValidator{}.validate(connection, mapping_);
        // Validate every live origin, including isolated vertices. Reference equality
        // follows the PK collation, but the catalog identity must match exact stored bytes.
        for (const auto &n: mapping_.nodes()) {
            const auto keys = frontier_keys(origins, n.type);
            if (keys.empty())
                continue;
            const auto rows = detail::pg_query(
                    connection,
                    frontier_sql(n.key_codec) + "SELECT frontier.ordinal,n." + quote_identifier(n.primary_key) +
                            " FROM frontier LEFT JOIN " + physical_table(n.table) + " n ON " +
                            pk_expression(n, "n", metadata) + " OPERATOR(pg_catalog.=) frontier.key",
                    true, {keys.dump()});
            if (static_cast<std::size_t>(PQntuples(rows.get())) != keys.size())
                throw std::invalid_argument("Ambiguous live frontier membership");
            for (int row = 0; row < PQntuples(rows.get()); ++row) {
                if (PQgetisnull(rows.get(), row, 1))
                    throw std::invalid_argument("Stale frontier: missing live vertex row");
                const auto ordinal = detail::sql_key(rows.get(), row, 0, KeyCodec::int64).get<std::int64_t>();
                if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= origins.size())
                    throw std::invalid_argument("Invalid frontier ordinal");
                const auto source = relational_source_identity(n, detail::sql_key(rows.get(), row, 1, n.key_codec));
                if (domain::encode_source_identity(source) != domain::encode_source_identity(origins[ordinal].source))
                    throw std::invalid_argument("Stale frontier: stored primary key differs from catalog identity");
            }
        }
        for (const auto &r: mapping_.relationships()) {
            if (r.kind != selected_kind || !allows(lookup.relationship_types, r.type))
                continue;
            const auto &from = mapping_.node(r.from_type), &to = mapping_.node(r.to_type);
            for (const bool reverse: {false, true}) {
                if ((reverse && lookup.direction == RelationshipLookupDirection::outgoing) ||
                    (!reverse && lookup.direction == RelationshipLookupDirection::incoming))
                    continue;
                if (!allows(lookup.target_types, reverse ? r.from_type : r.to_type))
                    continue;
                const auto keys = frontier_keys(origins, reverse ? r.to_type : r.from_type);
                if (keys.empty())
                    continue;
                auto text = r.kind == RelationshipKind::table ? table_query(r, from, to, reverse, metadata)
                                                              : foreign_key_query(r, from, to, reverse, metadata);
                (void) detail::pg_query(connection, "DECLARE polygraph_relationship_rows NO SCROLL CURSOR FOR " + text,
                                        false, {keys.dump()});
                for (;;) {
                    const auto rows = detail::pg_query(connection,
                                                       "FETCH FORWARD " + std::to_string(lookup.batch_size) +
                                                               " FROM polygraph_relationship_rows",
                                                       true);
                    const auto size = PQntuples(rows.get());
                    if (size == 0)
                        break;
                    std::vector<domain::SourceIdentity> endpoints;
                    endpoints.reserve(static_cast<std::size_t>(size) * 2);
                    std::vector<RelationalRelationshipRow> batch;
                    batch.reserve(size);
                    for (int row = 0; row < size; ++row) {
                        if (PQgetisnull(rows.get(), row, 3) || PQgetisnull(rows.get(), row, 4))
                            throw std::invalid_argument("Relationship '" + r.logical_type +
                                                        "' has NULL or missing live endpoints");
                        endpoints.push_back(
                                relational_source_identity(from, detail::sql_key(rows.get(), row, 3, from.key_codec)));
                        endpoints.push_back(
                                relational_source_identity(to, detail::sql_key(rows.get(), row, 4, to.key_codec)));
                        const auto ordinal = detail::sql_key(rows.get(), row, 0, KeyCodec::int64).get<std::int64_t>();
                        if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= origins.size())
                            throw std::invalid_argument("Invalid relationship origin ordinal");
                        auto properties = json::object();
                        for (std::size_t p = 0; p < r.properties.size(); ++p)
                            properties[r.properties[p].property] = detail::sql_property(
                                    rows.get(), row, static_cast<int>(p) + 6, r.properties[p].type);
                        batch.push_back({.origin = origins[ordinal].node,
                                         .type = r.type,
                                         .from = {},
                                         .to = {},
                                         .reverse = reverse,
                                         .external_key = r.primary_key ? std::optional<json>(detail::sql_key(
                                                                                 rows.get(), row, 5, *r.key_codec))
                                                                       : std::nullopt,
                                         .properties = std::move(properties)});
                    }
                    const auto resolved = identities_.resolve(endpoints);
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                        batch[i].from = resolved[i * 2];
                        batch[i].to = resolved[i * 2 + 1];
                    }
                    if (reverse && lookup.direction == RelationshipLookupDirection::both)
                        std::erase_if(batch,
                                      [](const auto &row) { return row.from.node.global_id == row.to.node.global_id; });
                    if (!batch.empty())
                        consume(batch);
                }
                (void) detail::pg_query(connection, "CLOSE polygraph_relationship_rows");
            }
        }
        snapshot.commit();
    }
} // namespace polygraph::postgres
