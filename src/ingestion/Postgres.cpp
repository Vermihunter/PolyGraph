#include "Native.hpp"
#if POLYGRAPH_INGESTION_HAS_LIBPQ
#include <array>
#include <libpq-fe.h>
#include <memory>
#include "IngestionSchema.hpp"

namespace polygraph::ingestion::detail {
    namespace {
        using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
        class Postgres final : public IImportExecutor {
        public:
            explicit Postgres(ImportContext context) : context_(std::move(context)), connection_(nullptr, PQfinish) {}
            void preflight() override {
                connect();
                ensure_empty();
            }
            Json run() override {
                connect();
                const auto start = Clock::now();
                command("BEGIN");
                auto acquired = query("SELECT pg_try_advisory_xact_lock(1886352231)");
                if (std::string(PQgetvalue(acquired.get(), 0, 0)) != "t")
                    throw Error("Another ingestion holds PostgreSQL target lock");
                command(schema_sql);
                command("LOCK TABLE public.polygraph_nodes,public.polygraph_edges IN ACCESS EXCLUSIVE MODE");
                ensure_empty();
                for (const auto &a: context_.artifacts.at("artifacts")) {
                    const bool nodes = a.at("kind") == "nodes";
                    const std::string columns =
                            nodes ? "polygraph_nodes(id,type,source,is_proxy,cache_residency,properties)"
                                  : "polygraph_edges(id,edge_type,source,from_id,from_type,to_id,to_type,properties)";
                    Result started(PQexec(connection_.get(),
                                          ("COPY public." + columns + " FROM STDIN WITH (FORMAT csv)").c_str()),
                                   PQclear);
                    if (!started || PQresultStatus(started.get()) != PGRES_COPY_IN)
                        fail("COPY could not start", started.get());
                    std::ifstream file(artifact_path(context_.root, a), std::ios::binary);
                    if (!file)
                        throw Error("Cannot read COPY artifact");
                    std::array<char, 1024 * 1024> buffer{};
                    while (file) {
                        file.read(buffer.data(), buffer.size());
                        if (file.gcount() > 0 &&
                            PQputCopyData(connection_.get(), buffer.data(), static_cast<int>(file.gcount())) != 1)
                            throw Error("PostgreSQL COPY send failed");
                    }
                    if (!file.eof())
                        throw Error("Read error in COPY artifact");
                    if (PQputCopyEnd(connection_.get(), nullptr) != 1)
                        throw Error("PostgreSQL COPY end failed");
                    Result done(PQgetResult(connection_.get()), PQclear);
                    if (!done || PQresultStatus(done.get()) != PGRES_COMMAND_OK)
                        fail("COPY failed", done.get());
                    if (std::string(PQcmdTuples(done.get())) != std::to_string(a.at("records").get<std::uint64_t>()))
                        throw Error("PostgreSQL COPY row count differs from artifact");
                    while (auto *pending = PQgetResult(connection_.get())) {
                        Result remaining(pending, PQclear);
                        if (PQresultStatus(remaining.get()) != PGRES_COMMAND_OK)
                            fail("COPY finalization failed", remaining.get());
                    }
                }
                if (context_.target.value("traversal_indexes", false))
                    command(R"(
                    CREATE INDEX IF NOT EXISTS polygraph_edges_from_idx ON public.polygraph_edges(from_id,edge_type) INCLUDE(to_id,to_type);
                    CREATE INDEX IF NOT EXISTS polygraph_edges_to_idx ON public.polygraph_edges(to_id,edge_type) INCLUDE(from_id,from_type);
                    CREATE INDEX IF NOT EXISTS polygraph_nodes_type_idx ON public.polygraph_nodes(type,id);
                )");
                command("ANALYZE public.polygraph_nodes; ANALYZE public.polygraph_edges");
                // Validate in the importing transaction; failures roll back both COPY streams.
                if (context_.verification == VerificationMode::full)
                    (void) verify();
                command("COMMIT");
                return {{"native_import_seconds", elapsed(start)}, {"mechanism", "libpq COPY FROM STDIN"}};
            }
            Json verify() override {
                connect();
                const auto expected = expected_counts(context_);
                auto counts = query(
                        "SELECT (SELECT count(*) FROM public.polygraph_nodes WHERE NOT is_proxy),(SELECT count(*) FROM "
                        "public.polygraph_nodes WHERE is_proxy),(SELECT count(*) FROM public.polygraph_edges)");
                for (int i = 0; i < 3; ++i) {
                    const auto label = std::array{"nodes", "proxies", "edges"}[i];
                    if (std::stoull(PQgetvalue(counts.get(), 0, i)) != expected.at(label).get<std::uint64_t>())
                        throw Error("PostgreSQL count mismatch for " + std::string(label));
                }
                auto missing = query("SELECT count(*) FROM public.polygraph_edges e LEFT JOIN public.polygraph_nodes f "
                                     "ON f.id=e.from_id LEFT JOIN public.polygraph_nodes t ON t.id=e.to_id WHERE f.id "
                                     "IS NULL OR t.id IS NULL");
                if (std::string(PQgetvalue(missing.get(), 0, 0)) != "0")
                    throw Error("PostgreSQL traversal has unresolved endpoints");
                verify_expected_rows(context_, [&](bool edge, const Json &batch) {
                    const std::string sql = edge ? "SELECT count(*) FROM jsonb_to_recordset($1::jsonb) AS x(id "
                                                   "bigint,\"from\" bigint,\"to\" bigint,type text,source text) JOIN "
                                                   "public.polygraph_edges e ON e.id=x.id AND e.from_id=x.\"from\" AND "
                                                   "e.to_id=x.\"to\" AND e.edge_type=x.type AND e.source=x.source"
                                                 : "SELECT count(*) FROM jsonb_to_recordset($1::jsonb) AS x(id "
                                                   "bigint,type text,proxy boolean) JOIN public.polygraph_nodes n ON "
                                                   "n.id=x.id AND n.type=x.type AND n.is_proxy=x.proxy";
                    const auto body = batch.dump();
                    const char *values[] = {body.c_str()};
                    Result result(PQexecParams(connection_.get(), sql.c_str(), 1, nullptr, values, nullptr, nullptr, 0),
                                  PQclear);
                    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
                        fail("PostgreSQL ID verification failed", result.get());
                    if (std::stoull(PQgetvalue(result.get(), 0, 0)) != batch.size())
                        throw Error("PostgreSQL canonical ID/type/endpoint mismatch");
                });
                return {{"counts", expected}, {"exact_ids", true}, {"endpoint_traversal", true}};
            }

        private:
            ImportContext context_;
            std::unique_ptr<PGconn, decltype(&PQfinish)> connection_;
            [[noreturn]] void fail(const std::string &message, PGresult *result) const {
                throw Error(message + ": " + (result ? std::string(PQresultErrorMessage(result)) : "no result"));
            }
            void connect() {
                if (connection_)
                    return;
                const auto password = environment_secret(context_.target, "password_env");
                const auto db = context_.target.at("database").get<std::string>();
                const auto host = context_.target.value("host", std::string("127.0.0.1"));
                const auto port = std::to_string(context_.target.value("port", 5432));
                const auto user = context_.target.value("username", std::string{});
                const char *keys[] = {
                        "host", "port", "dbname", "user", "password", "application_name", "connect_timeout", nullptr};
                const char *values[] = {host.c_str(),
                                        port.c_str(),
                                        db.c_str(),
                                        user.empty() ? nullptr : user.c_str(),
                                        password.empty() ? nullptr : password.c_str(),
                                        "polygraph-native-bulk-import",
                                        "15",
                                        nullptr};
                connection_.reset(PQconnectdbParams(keys, values, 0));
                if (!connection_ || PQstatus(connection_.get()) != CONNECTION_OK)
                    throw Error("Cannot connect to named PostgreSQL target (check host/port, libpq environment and "
                                "password_env)");
                command("SET client_encoding='UTF8'; SET search_path=pg_catalog,public; SET statement_timeout=0; SET "
                        "lock_timeout='15s'");
            }
            Result query(const std::string &sql) {
                Result result(PQexec(connection_.get(), sql.c_str()), PQclear);
                if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
                    fail("PostgreSQL query failed", result.get());
                return result;
            }
            void command(const std::string &sql) {
                Result result(PQexec(connection_.get(), sql.c_str()), PQclear);
                if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK)
                    fail("PostgreSQL command failed", result.get());
            }
            void ensure_empty() {
                auto other = query("SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE "
                                   "n.nspname NOT LIKE 'pg_%' AND n.nspname<>'information_schema' AND c.relkind IN "
                                   "('r','p','v','m','f','S') AND NOT (n.nspname='public' AND c.relname IN "
                                   "('polygraph_nodes','polygraph_edges') AND c.relkind='r')");
                if (std::string(PQgetvalue(other.get(), 0, 0)) != "0")
                    throw Error("PostgreSQL target is not isolated/empty (unrelated user relations exist)");
                auto tables = query("SELECT to_regclass('public.polygraph_nodes') IS NOT "
                                    "NULL,to_regclass('public.polygraph_edges') IS NOT NULL");
                for (int i = 0; i < 2; ++i)
                    if (std::string(PQgetvalue(tables.get(), 0, i)) == "t") {
                        const std::string table = i == 0 ? "polygraph_nodes" : "polygraph_edges";
                        auto count = query("SELECT EXISTS(SELECT 1 FROM public." + table + " LIMIT 1)");
                        if (std::string(PQgetvalue(count.get(), 0, 0)) != "f")
                            throw Error("PostgreSQL target already contains graph data; refusing to overwrite");
                    }
            }
        };
    } // namespace
    std::unique_ptr<IImportExecutor> postgres_executor(ImportContext c) {
        return std::make_unique<Postgres>(std::move(c));
    }
} // namespace polygraph::ingestion::detail
#else
namespace polygraph::ingestion::detail {
    std::unique_ptr<IImportExecutor> postgres_executor(ImportContext) {
        throw Error("PostgreSQL executor was built without libpq; rebuild with native executors enabled and libpq "
                    "installed");
    }
} // namespace polygraph::ingestion::detail
#endif
