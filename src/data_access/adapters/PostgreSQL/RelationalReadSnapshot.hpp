#pragma once

#include <algorithm>
#include <libpq-fe.h>
#include <memory>
#include "data_access/adapters/PostgreSQL/RelationalMapping.hpp"

namespace polygraph::postgres::detail {
    using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;
    inline PgResult pg_query(PGconn *connection, const std::string &sql, bool rows = false,
                             const std::vector<std::string> &parameters = {}) {
        std::vector<const char *> values;
        for (const auto &p: parameters)
            values.push_back(p.c_str());
        PgResult result(PQexecParams(connection, sql.c_str(), static_cast<int>(values.size()), nullptr, values.data(),
                                     nullptr, nullptr, 0),
                        PQclear);
        if (!result || PQresultStatus(result.get()) != (rows ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
            throw std::invalid_argument(
                    "PostgreSQL relational read: " +
                    std::string(result ? PQresultErrorMessage(result.get()) : PQerrorMessage(connection)));
        return result;
    }
    class RelationalReadSnapshot {
    public:
        RelationalReadSnapshot(PGconn *connection, const RelationalMapping &mapping) : connection_(connection) {
            if (!connection || PQstatus(connection) != CONNECTION_OK || PQtransactionStatus(connection) != PQTRANS_IDLE)
                throw std::invalid_argument("Relational reads require an idle PostgreSQL connection");
            (void) pg_query(connection_, "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
            active_ = true;
            try {
                (void) pg_query(connection_, "SET LOCAL client_encoding='UTF8'");
                // Utility LOCK commands precede any snapshot-taking SELECT. TRUNCATE
                // is not MVCC-safe; per-cursor table locking would omit rows.
                std::vector<std::string> tables;
                for (const auto &n: mapping.nodes())
                    tables.push_back(quote_identifier(n.table.schema) + "." + quote_identifier(n.table.name));
                for (const auto &r: mapping.relationships())
                    if (r.kind == RelationshipKind::table)
                        tables.push_back(quote_identifier(r.table.schema) + "." + quote_identifier(r.table.name));
                std::ranges::sort(tables);
                if (tables.empty())
                    throw std::invalid_argument("Relational snapshot requires node mappings");
                std::string sql = "LOCK TABLE ";
                for (std::size_t i = 0; i < tables.size(); ++i) {
                    if (i)
                        sql += ",";
                    sql += tables[i];
                }
                try {
                    (void) pg_query(connection_, sql + " IN ACCESS SHARE MODE");
                } catch (const std::invalid_argument &error) {
                    throw std::invalid_argument("Mapped-table snapshot locks require table-level SELECT privileges: " +
                                                std::string(error.what()));
                }
            } catch (...) {
                rollback();
                throw;
            }
        }
        ~RelationalReadSnapshot() { rollback(); }
        RelationalReadSnapshot(const RelationalReadSnapshot &) = delete;
        RelationalReadSnapshot &operator=(const RelationalReadSnapshot &) = delete;
        void commit() {
            (void) pg_query(connection_, "COMMIT");
            active_ = false;
        }

    private:
        void rollback() noexcept {
            if (active_) {
                PgResult r(PQexec(connection_, "ROLLBACK"), PQclear);
                active_ = false;
            }
        }
        PGconn *connection_;
        bool active_{false};
    };
} // namespace polygraph::postgres::detail
