#include "Native.hpp"
#include "ingestion/RelationalImport.hpp"
#if POLYGRAPH_INGESTION_HAS_LIBPQ
#include <fcntl.h>
#include <linux/fs.h>
#include <map>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>
#include "../data_access/adapters/PostgreSQL/RelationalSqlValue.hpp"
#include "data_access/adapters/PostgreSQL/RelationalAttachment.hpp"
#include "data_access/adapters/PostgreSQL/RelationalMappingValidator.hpp"
#include "domain/graph/LogicalGraphSchemaJson.hpp"

namespace polygraph::ingestion::detail {
    namespace {
        using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
        namespace pg = polygraph::postgres;
        std::string table(const pg::SqlTable &t) {
            return pg::quote_identifier(t.schema) + "." + pg::quote_identifier(t.name);
        }
        std::string sql_type(const pg::RelationalColumnMetadata &c) {
            switch (c.type_oid) {
                case 20:
                    return "pg_catalog.int8";
                case 21:
                    return "pg_catalog.int2";
                case 23:
                    return "pg_catalog.int4";
                case 25:
                case 1043:
                    return "pg_catalog.text";
                case 16:
                    return "pg_catalog.bool";
                case 700:
                    return "pg_catalog.float4";
                case 701:
                    return "pg_catalog.float8";
                case 1082:
                    return "pg_catalog.date";
                case 2950:
                    return "pg_catalog.uuid";
                case 1114:
                    return "pg_catalog.timestamp" +
                           (c.type_modifier >= 0 ? "(" + std::to_string(c.type_modifier) + ")" : "");
                case 1184:
                    return "pg_catalog.timestamptz" +
                           (c.type_modifier >= 0 ? "(" + std::to_string(c.type_modifier) + ")" : "");
                default:
                    throw Error("Unsupported native import SQL type");
            }
        }
        class RelationalPostgres final : public IImportExecutor {
            ImportContext context_;
            std::unique_ptr<PGconn, decltype(&PQfinish)> connection_{nullptr, PQfinish};
            domain::graph_t logical_;
            pg::RelationalMapping mapping_;
            std::optional<pg::ValidatedRelationalMapping> metadata_;
            fs::path output_, journal_;
            std::string binding_;
            std::set<std::string> checked_columns_;
            bool require_insert_{false};
            std::uint64_t max_record_bytes_{};
            Result query(const std::string &sql, const std::vector<std::string> &params = {}, bool rows = false) {
                std::vector<const char *> values;
                for (const auto &p: params)
                    values.push_back(p.c_str());
                Result r(PQexecParams(connection_.get(), sql.c_str(), static_cast<int>(values.size()), nullptr,
                                      values.data(), nullptr, nullptr, 0),
                         PQclear);
                if (!r || PQresultStatus(r.get()) != (rows ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
                    throw Error("Relational PostgreSQL: " +
                                std::string(r ? PQresultErrorMessage(r.get()) : PQerrorMessage(connection_.get())));
                return r;
            }
            void rollback() noexcept {
                if (connection_ && PQtransactionStatus(connection_.get()) != PQTRANS_IDLE) {
                    Result r(PQexec(connection_.get(), "ROLLBACK"), PQclear);
                }
            }
            void connect() {
                if (connection_)
                    return;
                const auto password = environment_secret(context_.target, "password_env"),
                           host = context_.target.value("host", std::string("127.0.0.1")),
                           port = std::to_string(context_.target.value("port", 5432)),
                           user = context_.target.value("username", std::string{}),
                           db = context_.target.at("database").get<std::string>();
                const char *keys[] = {"host", "port", "dbname", "user", "password", "connect_timeout", nullptr};
                const char *values[] = {host.c_str(),
                                        port.c_str(),
                                        db.c_str(),
                                        user.empty() ? nullptr : user.c_str(),
                                        password.empty() ? nullptr : password.c_str(),
                                        "15",
                                        nullptr};
                connection_.reset(PQconnectdbParams(keys, values, 0));
                if (!connection_ || PQstatus(connection_.get()) != CONNECTION_OK)
                    throw Error("Cannot connect to relational PostgreSQL target; check libpq environment");
            }
            void begin(bool writing, bool publishing = false) {
                connect();
                checked_columns_.clear();
                require_insert_ = writing;
                query(writing || publishing ? "BEGIN" : "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
                query("SET LOCAL client_encoding='UTF8'");
                query("SET LOCAL search_path=pg_catalog");
                query("SET LOCAL DateStyle='ISO,YMD'");
                query("SET LOCAL TimeZone='UTC'");
                query("SET LOCAL extra_float_digits=3");
                query("SET LOCAL lock_timeout='15s'");
                std::set<std::string> tables;
                for (const auto &n: mapping_.nodes())
                    tables.insert(table(n.table));
                for (const auto &r: mapping_.relationships())
                    if (r.kind == pg::RelationshipKind::table)
                        tables.insert(table(r.table));
                std::string lock = "LOCK TABLE ";
                for (const auto &t: tables) {
                    if (lock != "LOCK TABLE ")
                        lock += ",";
                    lock += t;
                }
                query(lock + (writing      ? " IN SHARE ROW EXCLUSIVE MODE"
                              : publishing ? " IN SHARE MODE"
                                           : " IN ACCESS SHARE MODE"));
                metadata_ = pg::RelationalMappingValidator{}.validate(connection_.get(), mapping_);
            }
            const pg::NodeMapping &node(const std::string &type) const {
                return mapping_.node(logical_->node_type_id(type));
            }
            const pg::RelationalTableMetadata &meta(const pg::NodeMapping &n) const {
                for (const auto &t: metadata_->tables())
                    if (t.table == n.table)
                        return t;
                throw Error("Missing native table metadata");
            }
            void empty() {
                for (const auto &n: mapping_.nodes()) {
                    auto r = query("SELECT EXISTS(SELECT 1 FROM " + table(n.table) + " LIMIT 1)", {}, true);
                    if (std::string(PQgetvalue(r.get(), 0, 0)) == "t")
                        throw Error("Mapped node destination is not empty: " + table(n.table));
                }
            }
            void check_row(const Json &r) {
                const auto &n = node(r.at("type"));
                const auto &m = meta(n);
                const auto &columns = r.at("columns"), &values = r.at("values");
                if (!columns.is_array() || columns.empty() || columns.size() != values.size() ||
                    columns[0] != n.primary_key)
                    throw Error("Invalid relational artifact columns");
                std::set<std::string> present;
                for (std::size_t i = 0; i < columns.size(); ++i) {
                    const auto col = columns[i].get<std::string>();
                    if (!present.insert(col).second)
                        throw Error("Duplicate native column");
                    bool allowed = col == n.primary_key;
                    for (const auto &p: n.properties)
                        allowed = allowed || p.column == col;
                    if (!allowed || !m.columns.contains(col))
                        throw Error("Unmapped native write column");
                    const auto &c = m.columns.at(col);
                    if (c.generated || !c.identity.empty() || (col == n.primary_key && c.has_default))
                        throw Error("Explicit import cannot write generated/automatic primary-key columns: " + col);
                    const auto &v = values[i];
                    if (v.is_null()) {
                        if (!c.nullable)
                            throw Error("NULL in required native column: " + col);
                    } else {
                        if (!v.is_string() || v.get_ref<const std::string &>().find('\0') != std::string::npos ||
                            v.get_ref<const std::string &>().size() > 1024 * 1024)
                            throw Error("Invalid/oversized native scalar");
                        if (c.type_oid == 1043 && c.type_modifier >= 4) {
                            std::size_t length = 0;
                            for (unsigned char ch: v.get_ref<const std::string &>())
                                if ((ch & 0xc0) != 0x80)
                                    ++length;
                            if (length > static_cast<std::size_t>(c.type_modifier - 4))
                                throw Error("Native varchar overflow: " + col);
                        }
                    }
                    if (require_insert_ && checked_columns_.insert(std::to_string(m.oid) + ":" + col).second) {
                        auto privilege = query(
                                "SELECT "
                                "pg_catalog.has_column_privilege($1::pg_catalog.oid,$2::pg_catalog.text,'INSERT')",
                                {std::to_string(m.oid), col}, true);
                        if (std::string(PQgetvalue(privilege.get(), 0, 0)) != "t")
                            throw Error("INSERT privilege missing for " + col);
                    }
                }
                for (const auto &[name, c]: m.columns)
                    if (!c.nullable && !c.generated && !c.has_default && c.identity.empty() && !present.contains(name))
                        throw Error("Missing required native column: " + name);
            }
            template<class F>
            void groups(F action) {
                std::map<std::string, Json> groups;
                std::size_t bytes = 0, count = 0;
                const auto flush = [&] {
                    for (const auto &[key, batch]: groups) {
                        (void) key;
                        action(batch);
                    }
                    groups.clear();
                    bytes = count = 0;
                };
                for (const auto &a: context_.artifacts.at("artifacts")) {
                    if (a.at("kind") != "relational_nodes")
                        throw Error("Non-relational artifact in mapped import");
                    std::ifstream input(artifact_path(context_.root, a));
                    if (!input)
                        throw Error("Cannot read mapped artifact");
                    std::string line;
                    std::uint64_t records = 0;
                    while (std::getline(input, line)) {
                        if (line.size() + 1 > max_record_bytes_)
                            throw Error("Relational artifact row exceeds prepared max-record-mb");
                        if (count && (bytes + line.size() > 4 * 1024 * 1024 || count >= 128))
                            flush();
                        auto row = Json::parse(line);
                        const auto group = Json::array({row.at("type"), row.at("columns")}).dump();
                        if (!groups.contains(group))
                            groups[group] = Json::array();
                        groups[group].push_back(std::move(row));
                        bytes += line.size();
                        ++count;
                        ++records;
                    }
                    if (!input.eof() || records != a.at("records").get<std::uint64_t>())
                        throw Error("Mapped artifact row count/read mismatch");
                }
                flush();
            }
            Result expected(const Json &batch) {
                const auto &n = node(batch[0].at("type"));
                const auto &m = meta(n);
                const auto &columns = batch[0].at("columns");
                Json values = Json::array();
                for (const auto &r: batch) {
                    check_row(r);
                    values.push_back(r.at("values"));
                }
                std::string sql = "SELECT ";
                for (std::size_t i = 0; i < columns.size(); ++i) {
                    if (i)
                        sql += ",";
                    sql += "(v OPERATOR(pg_catalog.->>) " + std::to_string(i) +
                           ")::" + sql_type(m.columns.at(columns[i].get<std::string>()));
                }
                sql += " FROM pg_catalog.jsonb_array_elements($1::pg_catalog.jsonb) AS input(v)";
                return query(sql, {values.dump()}, true);
            }
            void copy(const Json &batch) {
                const auto &n = node(batch[0].at("type"));
                std::string sql = "COPY " + table(n.table) + "(";
                for (std::size_t i = 0; i < batch[0]["columns"].size(); ++i) {
                    if (i)
                        sql += ",";
                    sql += pg::quote_identifier(batch[0]["columns"][i].get<std::string>());
                }
                sql += ") FROM STDIN WITH (FORMAT csv, NULL '\\N')";
                Result start(PQexec(connection_.get(), sql.c_str()), PQclear);
                if (!start || PQresultStatus(start.get()) != PGRES_COPY_IN)
                    throw Error("Relational COPY could not start");
                bool failed = false;
                for (const auto &r: batch) {
                    std::string line;
                    for (std::size_t i = 0; i < r["values"].size(); ++i) {
                        if (i)
                            line += ",";
                        const auto &v = r["values"][i];
                        if (v.is_null())
                            line += "\\N";
                        else {
                            line += '"';
                            for (char c: v.get_ref<const std::string &>()) {
                                if (c == '"')
                                    line += '"';
                                line += c;
                            }
                            line += '"';
                        }
                    }
                    line += '\n';
                    if (PQputCopyData(connection_.get(), line.data(), static_cast<int>(line.size())) != 1)
                        failed = true;
                }
                if (PQputCopyEnd(connection_.get(), failed ? "COPY send failed" : nullptr) != 1)
                    failed = true;
                std::string error;
                while (auto *raw = PQgetResult(connection_.get())) {
                    Result r(raw, PQclear);
                    if (PQresultStatus(r.get()) != PGRES_COMMAND_OK)
                        error = PQresultErrorMessage(r.get());
                }
                if (failed || !error.empty())
                    throw Error("Relational COPY failed: " + error);
            }
            void verify_rows() {
                std::map<std::string, std::uint64_t> counts;
                groups([&](const Json &batch) {
                    const auto canonical = expected(batch);
                    const auto &n = node(batch[0].at("type"));
                    const auto &m = meta(n);
                    const auto &columns = batch[0].at("columns");
                    Json keys = Json::array();
                    for (const auto &r: batch)
                        keys.push_back(r["values"][0]);
                    std::string sql = "SELECT k.ordinality";
                    for (const auto &col: columns)
                        sql += ",n." + pg::quote_identifier(col.get<std::string>());
                    std::string pk = "n." + pg::quote_identifier(n.primary_key);
                    const auto &collation = m.columns.at(n.primary_key).collation;
                    if (collation)
                        pk += " COLLATE " + table(*collation);
                    sql += " FROM pg_catalog.jsonb_array_elements_text($1::pg_catalog.jsonb) WITH ORDINALITY AS "
                           "k(value,ordinality) LEFT JOIN " +
                           table(n.table) + " n ON " + pk +
                           " OPERATOR(pg_catalog.=) k.value::" + sql_type(m.columns.at(n.primary_key)) +
                           " ORDER BY k.ordinality";
                    auto actual = query(sql, {keys.dump()}, true);
                    if (PQntuples(actual.get()) != static_cast<int>(batch.size()))
                        throw Error("Native row membership mismatch");
                    for (int row = 0; row < PQntuples(actual.get()); ++row) {
                        if (PQgetisnull(actual.get(), row, 1))
                            throw Error("Missing imported native PK");
                        for (std::size_t col = 0; col < columns.size(); ++col) {
                            if (PQgetisnull(actual.get(), row, static_cast<int>(col) + 1) !=
                                PQgetisnull(canonical.get(), row, static_cast<int>(col)))
                                throw Error("Native NULL mismatch");
                            if (!PQgetisnull(canonical.get(), row, static_cast<int>(col)) &&
                                std::string(PQgetvalue(actual.get(), row, static_cast<int>(col) + 1)) !=
                                        PQgetvalue(canonical.get(), row, static_cast<int>(col)))
                                throw Error("Imported native column mismatch");
                        }
                        ++counts[n.logical_type];
                    }
                });
                for (const auto &n: mapping_.nodes()) {
                    auto r = query("SELECT count(*) FROM " + table(n.table), {}, true);
                    if (std::stoull(PQgetvalue(r.get(), 0, 0)) != counts[n.logical_type])
                        throw Error("Native destination count differs from prepared nodes");
                }
            }
            void verify_catalog(const fs::path &manifest) {
                const pg::RelationalIdentityResolver resolver(manifest, mapping_);
                if (read_json(manifest).at("counts").at("nodes") != context_.artifacts.at("counts").at("nodes"))
                    throw Error("Published catalog count differs from prepared nodes");
                groups([&](const Json &batch) {
                    const auto canonical = expected(batch);
                    const auto &n = node(batch[0].at("type"));
                    std::vector<domain::SourceIdentity> sources;
                    for (int row = 0; row < PQntuples(canonical.get()); ++row)
                        sources.push_back(pg::relational_source_identity(
                                n, pg::detail::sql_key(canonical.get(), row, 0, n.key_codec)));
                    const auto resolved = resolver.resolve(sources);
                    for (std::size_t i = 0; i < resolved.size(); ++i)
                        if (resolved[i].node.global_id != std::bit_cast<Id>(batch[i].at("id").get<std::int64_t>()))
                            throw Error("Published catalog ID differs from prepared canonical ID");
                });
            }
            Json publish() {
                const auto staged = output_.parent_path() / (".polygraph-" + binding_ + ".staged");
                auto journal = read_json(journal_);
                const bool published = fs::exists(output_);
                if (published) {
                    if (!journal.contains("catalog_generation") || !fs::exists(output_ / "manifest.json") ||
                        read_json(output_ / "manifest.json").at("generation_id") != journal.at("catalog_generation"))
                        throw Error("Identity output already exists; refusing overwrite");
                } else {
                    // Attachment publishes its manifest last. A task-owned directory without
                    // that marker is an interrupted build, never a usable catalog generation.
                    if (fs::exists(staged) && !fs::exists(staged / "manifest.json")) {
                        if (fs::is_symlink(staged) || !fs::is_directory(staged))
                            throw Error("Invalid relational staging directory");
                        fs::remove_all(staged);
                    }
                    if (!fs::exists(staged))
                        (void) pg::attach_relational_nodes(
                                connection_.get(), mapping_,
                                {.output_dir = staged, .source_manifest = context_.root / "manifest.json"});
                }
                try {
                    // Keep application rows stable through verification, rename and the durable
                    // publication marker. Ordinary reads remain available during publication.
                    begin(false, true);
                    if (!published) {
                        verify_rows();
                        verify_catalog(staged / "manifest.json");
                        journal["catalog_generation"] = read_json(staged / "manifest.json").at("generation_id");
                        journal["state"] = "catalog_staged";
                        write_json_atomic(journal_, journal);
                        if (::syscall(SYS_renameat2, AT_FDCWD, staged.c_str(), AT_FDCWD, output_.c_str(),
                                      RENAME_NOREPLACE) != 0)
                            throw Error("Cannot atomically publish relational identity directory; recover explicitly");
                    }
                    const int fd = ::open(output_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    if (fd < 0)
                        throw Error("Cannot fsync identity parent directory");
                    const auto synced = ::fsync(fd);
                    ::close(fd);
                    if (synced != 0)
                        throw Error("Cannot fsync identity publication");
                    verify_rows();
                    verify_catalog(output_ / "manifest.json");
                    journal["state"] = "published";
                    journal["manifest"] = (output_ / "manifest.json").string();
                    write_json_atomic(journal_, journal);
                    query("COMMIT");
                } catch (...) {
                    rollback();
                    throw;
                }
                return {{"identity_manifest", (output_ / "manifest.json").string()},
                        {"counts", context_.artifacts.at("counts")},
                        {"exact_ids", true}};
            }


        public:
            explicit RelationalPostgres(ImportContext c) : context_(std::move(c)) {
                const auto prepared = read_json(context_.root / "manifest.json");
                max_record_bytes_ = prepared.at("limits").at("max_record_mb").get<std::uint64_t>() * 1024 * 1024;
                const auto &config = context_.artifacts.at("relational");
                if (config != prepared.at("configuration").at("relational_postgresql"))
                    throw Error("Relational artifact configuration mismatch");
                logical_ = domain::logical_schema_from_json(config.at("schema"));
                mapping_ = pg::parse_relational_mapping(config.at("mapping"), *logical_);
                if (config.at("mapping_sha256") != pg::relational_mapping_fingerprint(mapping_) ||
                    read_json(context_.target.at("relationalMapping")) != config.at("mapping"))
                    throw Error("Relational target mapping differs from prepared mapping");
                if (context_.verification != VerificationMode::full)
                    throw Error("Relational node imports require full verification");
                output_ = context_.target.at("relationalIdentityOutput").get<std::string>();
                if (!output_.is_absolute())
                    throw Error("relationalIdentityOutput must resolve to an absolute path");
                if (context_.artifacts.at("counts").at("edges") != 0 ||
                    context_.artifacts.at("counts").at("proxies") != 0)
                    throw Error("Relational relationship/proxy ingestion is not supported by #18");
                binding_ = sha256_text(Json{{"generation", prepared.at("generation_id")},
                                            {"target", context_.target},
                                            {"mapping", config.at("mapping_sha256")}}
                                               .dump());
                journal_ = context_.root / "relational-postgresql-publication.json";
            }
            ~RelationalPostgres() override { rollback(); }
            void preflight() override {
                try {
                    begin(false);
                    for (const auto &t: metadata_->tables()) {
                        const auto privilege =
                                query("SELECT pg_catalog.has_table_privilege($1::pg_catalog.oid,'UPDATE')",
                                      {std::to_string(t.oid)}, true);
                        if (std::string(PQgetvalue(privilege.get(), 0, 0)) != "t")
                            throw Error("Table UPDATE privilege required for import/publication locks: " +
                                        table(t.table));
                    }
                    require_insert_ = true;
                    empty();
                    groups([&](const Json &batch) { (void) expected(batch); });
                    query("COMMIT");
                } catch (...) {
                    rollback();
                    throw;
                }
            }
            Json run() override {
                if (fs::exists(journal_))
                    throw Error("Relational publication journal exists; use explicit recovery, never reimport");
                try {
                    begin(true);
                    empty();
                    groups([&](const Json &batch) {
                        (void) expected(batch);
                        copy(batch);
                    });
                    verify_rows();
                    write_json_atomic(journal_, {{"binding", binding_}, {"state", "pending_commit"}});
                    query("COMMIT");
                    write_json_atomic(journal_, {{"binding", binding_}, {"state", "data_committed"}});
                    return publish();
                } catch (...) {
                    rollback();
                    throw;
                }
            }
            Json verify() override {
                try {
                    begin(false);
                    verify_rows();
                    query("COMMIT");
                    if (!fs::exists(journal_) || read_json(journal_).value("state", std::string{}) != "published" ||
                        read_json(journal_).at("binding") != binding_)
                        throw Error("Relational identity publication incomplete; recover explicitly");
                    begin(false);
                    verify_catalog(output_ / "manifest.json");
                    query("COMMIT");
                    return {{"counts", context_.artifacts.at("counts")},
                            {"exact_ids", true},
                            {"identity_manifest", (output_ / "manifest.json").string()}};
                } catch (...) {
                    rollback();
                    throw;
                }
            }
            Json recover() {
                if (!fs::exists(journal_) || read_json(journal_).at("binding") != binding_)
                    throw Error("No matching relational publication journal");
                if (read_json(journal_).value("state", std::string{}) == "published")
                    return verify();
                try {
                    begin(false);
                    verify_rows();
                    query("COMMIT");
                    return publish();
                } catch (...) {
                    rollback();
                    throw;
                }
            }
        };
    } // namespace
    std::unique_ptr<IImportExecutor> postgres_relational_executor(ImportContext c) {
        return std::make_unique<RelationalPostgres>(std::move(c));
    }
    Json recover_relational(ImportContext c) { return RelationalPostgres(std::move(c)).recover(); }
} // namespace polygraph::ingestion::detail
#else
namespace polygraph::ingestion::detail {
    std::unique_ptr<IImportExecutor> postgres_relational_executor(ImportContext) {
        throw Error("Relational ingestion requires native libpq support");
    }
    Json recover_relational(ImportContext) { throw Error("Relational ingestion requires native libpq support"); }
} // namespace polygraph::ingestion::detail
#endif
namespace polygraph::ingestion {
    Json recover_postgres_relational_identity(const fs::path &manifest_path, const fs::path &target_path) {
        const auto root = fs::absolute(manifest_path).parent_path();
        detail::DirectoryLock lock(root);
        const auto m = verify(manifest_path, true);
        const auto config = detail::resolved_relational_targets(read_json(target_path), target_path);
        const std::array<Backend, 1> backends{Backend::postgresql};
        const auto targets = detail::checked_targets(config, backends);
        return detail::recover_relational({.backend = Backend::postgresql,
                                           .root = root,
                                           .artifacts = m.at("backends").at("PostgreSQL"),
                                           .target = targets.at("PostgreSQL")});
    }
} // namespace polygraph::ingestion
