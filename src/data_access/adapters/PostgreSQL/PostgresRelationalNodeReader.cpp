#include "data_access/adapters/PostgreSQL/PostgresRelationalNodeReader.hpp"
#include "RelationalFrontier.hpp"
#include "RelationalReadSnapshot.hpp"
#include "RelationalReaderMapping.hpp"
#include "RelationalSqlValue.hpp"
#include "data_model/Value.hpp"

namespace polygraph::postgres {
    namespace {
        value_t native_value(const json &value) {
            if (value.is_null())
                return std::make_unique<Value>();
            if (value.is_boolean())
                return std::make_unique<Value>(value.get<bool>());
            if (value.is_number_integer())
                return std::make_unique<Value>(value.get<std::int64_t>());
            if (value.is_number_float())
                return std::make_unique<Value>(value.get<double>());
            if (value.is_string())
                return std::make_unique<Value>(value.get<std::string>());
            throw std::invalid_argument("Unsupported non-scalar native node property");
        }
    } // namespace
    PostgresRelationalNodeReader::PostgresRelationalNodeReader(RelationalMapping mapping,
                                                               const RelationalIdentityResolver &identities) :
        mapping_(std::move(mapping)), identities_(identities) {
        detail::validate_read_mapping(mapping_, identities_);
    }

    std::vector<record_t> PostgresRelationalNodeReader::read_nodes(PGconn *connection,
                                                                   std::span<const NodeRef> requested) const {
        if (!connection || PQstatus(connection) != CONNECTION_OK || PQtransactionStatus(connection) != PQTRANS_IDLE)
            throw std::invalid_argument("Node reads require an idle PostgreSQL connection");
        if (requested.size() > RelationalIdentityResolver::max_batch_size)
            throw std::invalid_argument("Node read request exceeds 4096 vertices");
        if (requested.empty())
            return {};
        const auto origins = identities_.reverse(requested);
        detail::RelationalReadSnapshot snapshot(connection, mapping_);
        (void) detail::pg_query(connection, "SET LOCAL DateStyle='ISO,YMD'");
        (void) detail::pg_query(connection, "SET LOCAL TimeZone='UTC'");
        (void) detail::pg_query(connection, "SET LOCAL extra_float_digits=3");
        const auto metadata = RelationalMappingValidator{}.validate(connection, mapping_);
        std::vector<record_t> result(requested.size());
        for (const auto &n: mapping_.nodes()) {
            const auto keys = detail::frontier_keys(origins, n.type, false);
            if (keys.empty())
                continue;
            std::string query =
                    detail::frontier_sql(n.key_codec) + "SELECT frontier.ordinal,n." + quote_identifier(n.primary_key);
            for (const auto &p: n.properties)
                query += ",n." + quote_identifier(p.column);
            query += " FROM frontier LEFT JOIN " + detail::physical_table(n.table) + " n ON " +
                     detail::pk_expression(n, "n", metadata) + " OPERATOR(pg_catalog.=) frontier.key";
            (void) detail::pg_query(connection, "DECLARE polygraph_node_rows NO SCROLL CURSOR FOR " + query, false,
                                    {keys.dump()});
            for (;;) {
                const auto rows = detail::pg_query(connection, "FETCH FORWARD 1024 FROM polygraph_node_rows", true);
                if (PQntuples(rows.get()) == 0)
                    break;
                for (int row = 0; row < PQntuples(rows.get()); ++row) {
                    if (PQgetisnull(rows.get(), row, 1))
                        throw std::invalid_argument("Missing live node row for type '" + n.logical_type + "'");
                    const auto ordinal = detail::sql_key(rows.get(), row, 0, KeyCodec::int64).get<std::int64_t>();
                    if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= origins.size() ||
                        origins[ordinal].node.type != n.type || result[ordinal])
                        throw std::invalid_argument("Ambiguous or invalid native node read ordinal");
                    const auto source = relational_source_identity(n, detail::sql_key(rows.get(), row, 1, n.key_codec));
                    if (domain::encode_source_identity(source) !=
                        domain::encode_source_identity(origins[ordinal].source))
                        throw std::invalid_argument(
                                "Stored primary key differs from catalog identity; refresh required");
                    Node node{.global_id = origins[ordinal].node.global_id,
                              .type = n.type,
                              .labels = {n.logical_type},
                              .properties = {}};
                    for (std::size_t p = 0; p < n.properties.size(); ++p) {
                        const auto &property = n.properties[p];
                        try {
                            node.properties.emplace(property.property,
                                                    native_value(detail::sql_property(
                                                            rows.get(), row, static_cast<int>(p) + 2, property.type)));
                        } catch (const std::invalid_argument &error) {
                            throw std::invalid_argument("Node '" + n.logical_type + "' property '" + property.property +
                                                        "' at " + detail::physical_table(n.table) + "." +
                                                        quote_identifier(property.column) + ": " + error.what());
                        }
                    }
                    auto record = std::make_unique<Record>();
                    record->fields.emplace_back(std::move(node));
                    result[ordinal] = std::move(record);
                }
            }
            (void) detail::pg_query(connection, "CLOSE polygraph_node_rows");
        }
        for (const auto &record: result)
            if (!record)
                throw std::invalid_argument("Node read did not resolve every requested vertex");
        snapshot.commit();
        return result;
    }
} // namespace polygraph::postgres
