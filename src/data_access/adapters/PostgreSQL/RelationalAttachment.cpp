#include "data_access/adapters/PostgreSQL/RelationalAttachment.hpp"
#include "RelationalCatalogInternal.hpp"
#include "RelationalReadSnapshot.hpp"
#include "data_access/adapters/PostgreSQL/RelationalMappingValidator.hpp"

#include <algorithm>
#include <charconv>
#include <fcntl.h>
#include <unistd.h>

namespace polygraph::postgres {
    namespace {
        using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
        Result sql(PGconn *c, const std::string &text, bool rows = false) {
            Result r(PQexec(c, text.c_str()), PQclear);
            if (!r || PQresultStatus(r.get()) != (rows ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
                detail::fail("PostgreSQL snapshot operation failed: " +
                             std::string(r ? PQresultErrorMessage(r.get()) : PQerrorMessage(c)));
            return r;
        }
        void sync_path(const std::filesystem::path &p) {
            const auto fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                detail::fail("cannot open artifact for fsync");
            const auto status = ::fsync(fd);
            ::close(fd);
            if (status != 0)
                detail::fail("cannot fsync identity artifacts");
        }
        void metadata(sqlite3 *db, const std::string &key, const std::string &value) {
            const auto s = detail::statement(db, "INSERT OR REPLACE INTO metadata VALUES(?,?)");
            detail::bind(s.get(), 1, key);
            detail::bind(s.get(), 2, value);
            detail::run(s.get());
        }
        std::uint64_t next_payload(sqlite3 *db) {
            const auto s = detail::statement(db, "SELECT value FROM metadata WHERE key='next_payload'");
            if (sqlite3_step(s.get()) != SQLITE_ROW)
                detail::fail("catalog lacks allocation watermark");
            const auto text = detail::text(s.get(), 0);
            std::uint64_t value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value > (GlobalId{1} << 48))
                detail::fail("invalid catalog allocation watermark");
            return value;
        }
        void reserve_id(GlobalId id, const std::string &owner, std::uint64_t &next) {
            if (insertion::BackendEncodedId::canonical_id(id) != id ||
                insertion::BackendEncodedId::decode(id, detail::routing) != detail::catalog_owner(owner))
                detail::fail("source catalog owner/ID mismatch");
            next = std::max(next, (id & insertion::BackendEncodedId::payload_mask()) + 1);
        }
        void insert_identity(sqlite3_stmt *insert, GlobalId id, const std::string &key, const std::string &type,
                             const std::string &owner) {
            detail::bind(insert, 1, global_id_to_signed_storage(id));
            detail::bind(insert, 2, key);
            detail::bind(insert, 3, type);
            detail::bind(insert, 4, owner);
            detail::run(insert);
        }
    } // namespace
    std::filesystem::path attach_relational_nodes(PGconn *connection, const RelationalMapping &mapping,
                                                  const RelationalAttachmentOptions &options) {
        if (!connection || PQstatus(connection) != CONNECTION_OK || PQtransactionStatus(connection) != PQTRANS_IDLE)
            detail::fail("attachment requires an idle PostgreSQL connection");
        if (mapping.nodes().empty())
            detail::fail("attachment requires node mappings");
        if (options.output_dir.empty() || options.batch_size == 0 ||
            options.batch_size > RelationalIdentityResolver::max_batch_size)
            detail::fail("output directory and batch size 1..4096 are required");
        if (options.previous_manifest && options.source_manifest)
            detail::fail("choose previous generation or prepared source index, not both");
        const auto fingerprint = relational_mapping_fingerprint(mapping);
        std::optional<detail::CatalogFile> previous, source;
        if (options.previous_manifest) {
            const RelationalIdentityResolver check(*options.previous_manifest, mapping);
            previous = detail::read_catalog_file(*options.previous_manifest, true);
        }
        if (options.source_manifest) {
            source = detail::read_catalog_file(*options.source_manifest, false);
            if (source->manifest.value("format", std::string{}) == "polygraph.relational-identity")
                detail::fail("relational catalogs require --previous, not an ingestion --source-index");
        }
        const auto root = std::filesystem::absolute(options.output_dir).lexically_normal();
        std::filesystem::create_directories(root.parent_path());
        if (!std::filesystem::create_directory(root))
            detail::fail("output generation already exists; refusing overwrite");
        try {
            const auto index = root / "catalog.sqlite3";
            std::uint64_t next{};
            std::uint64_t count{};
            {
                auto db = detail::open(index, false);
                if (previous) {
                    const auto old = detail::open(previous->index, true);
                    auto *backup = sqlite3_backup_init(db.get(), "main", old.get(), "main");
                    if (!backup)
                        detail::fail("cannot copy previous catalog");
                    const auto status = sqlite3_backup_step(backup, -1);
                    const auto finish = sqlite3_backup_finish(backup);
                    if (status != SQLITE_DONE || finish != SQLITE_OK)
                        detail::fail("copying previous catalog failed");
                    next = next_payload(db.get());
                } else {
                    detail::exec(db.get(), R"(
                        CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;
                        CREATE TABLE identities(id INTEGER PRIMARY KEY,key TEXT NOT NULL UNIQUE,type TEXT NOT NULL,owner TEXT NOT NULL,
                            active INTEGER NOT NULL CHECK(active IN (0,1)),seen INTEGER NOT NULL CHECK(seen IN (0,1)));
                        CREATE VIEW nodes AS SELECT id,key,type,owner FROM identities WHERE active=1;
                    )");
                }
                detail::exec(db.get(), "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA cache_size=-8192; "
                                       "PRAGMA temp_store=FILE; BEGIN IMMEDIATE;");
                metadata(db.get(), "mapping_sha256", fingerprint);
                const auto insert = detail::statement(db.get(), "INSERT INTO identities VALUES(?,?,?,?,1,1)");
                if (previous) {
                    const auto rows = detail::statement(db.get(), "SELECT id,key,type,owner FROM identities");
                    int status;
                    while ((status = sqlite3_step(rows.get())) == SQLITE_ROW) {
                        const auto id = detail::id(rows.get(), 0);
                        const auto source_key = detail::decode_key(detail::text(rows.get(), 1));
                        const auto &n = detail::node_by_name(mapping, detail::text(rows.get(), 2));
                        if (source_key.name_space != n.name_space || source_key.type != n.logical_type ||
                            domain::encode_source_identity(relational_source_identity(n, source_key.value)) !=
                                    detail::text(rows.get(), 1))
                            detail::fail("invalid historical source identity");
                        std::uint64_t reserved{};
                        reserve_id(id, detail::text(rows.get(), 3), reserved);
                        if (reserved > next)
                            detail::fail("catalog allocation watermark precedes historical IDs");
                    }
                    if (status != SQLITE_DONE)
                        detail::fail("cannot read historical identities");
                }
                if (source) {
                    const auto seed = detail::open(source->index, true);
                    // Reserve every prepared entity ID, including relationships and unmapped types.
                    const auto all_ids = detail::statement(seed.get(), "SELECT id FROM identities");
                    int status;
                    while ((status = sqlite3_step(all_ids.get())) == SQLITE_ROW) {
                        const auto id = detail::id(all_ids.get(), 0);
                        if (insertion::BackendEncodedId::canonical_id(id) != id)
                            detail::fail("noncanonical prepared identity");
                        (void) insertion::BackendEncodedId::decode(id, detail::routing);
                        next = std::max(next, (id & insertion::BackendEncodedId::payload_mask()) + 1);
                    }
                    if (status != SQLITE_DONE)
                        detail::fail("cannot read prepared entity IDs");
                    const auto rows = detail::statement(seed.get(), "SELECT id,key,type,owner FROM nodes");
                    while ((status = sqlite3_step(rows.get())) == SQLITE_ROW) {
                        const auto type = detail::text(rows.get(), 2);
                        const NodeMapping *mapped = nullptr;
                        for (const auto &n: mapping.nodes())
                            if (n.logical_type == type)
                                mapped = &n;
                        if (!mapped)
                            continue;
                        const auto key = detail::text(rows.get(), 1);
                        const auto identity = detail::decode_key(key);
                        if (identity.name_space != mapped->name_space)
                            continue;
                        if (identity.type != type ||
                            domain::encode_source_identity(relational_source_identity(*mapped, identity.value)) != key)
                            detail::fail("prepared source key differs from relational canonical codec");
                        const auto id = detail::id(rows.get(), 0);
                        const auto owner = detail::text(rows.get(), 3);
                        reserve_id(id, owner, next);
                        insert_identity(insert.get(), id, key, type, owner);
                    }
                    if (status != SQLITE_DONE)
                        detail::fail("cannot read prepared source identities");
                }
                detail::exec(db.get(), "UPDATE identities SET seen=0; CREATE TABLE scan(key TEXT PRIMARY KEY,type TEXT "
                                       "NOT NULL) WITHOUT ROWID;");
                const auto scan = detail::statement(db.get(), "INSERT INTO scan VALUES(?,?)");
                detail::RelationalReadSnapshot snapshot(connection, mapping);
                (void) RelationalMappingValidator{}.validate(connection, mapping);
                for (const auto &n: mapping.nodes()) {
                    sql(connection, "DECLARE polygraph_identity_scan NO SCROLL CURSOR FOR SELECT " +
                                            quote_identifier(n.primary_key) + " FROM " +
                                            quote_identifier(n.table.schema) + "." + quote_identifier(n.table.name));
                    for (;;) {
                        auto rows = sql(connection,
                                        "FETCH FORWARD " + std::to_string(options.batch_size) +
                                                " FROM polygraph_identity_scan",
                                        true);
                        if (PQntuples(rows.get()) == 0)
                            break;
                        for (int row = 0; row < PQntuples(rows.get()); ++row) {
                            if (PQgetisnull(rows.get(), row, 0) || PQgetlength(rows.get(), row, 0) > 1024 * 1024)
                                detail::fail("NULL or oversized SQL primary key");
                            const std::string text(PQgetvalue(rows.get(), row, 0),
                                                   static_cast<std::size_t>(PQgetlength(rows.get(), row, 0)));
                            json value = text;
                            if (n.key_codec == KeyCodec::int64) {
                                std::int64_t integer{};
                                const auto [end, error] =
                                        std::from_chars(text.data(), text.data() + text.size(), integer);
                                if (error != std::errc{} || end != text.data() + text.size())
                                    detail::fail("invalid PostgreSQL integer key");
                                value = integer;
                            }
                            const auto identity = relational_source_identity(n, value);
                            detail::bind(scan.get(), 1, domain::encode_source_identity(identity));
                            detail::bind(scan.get(), 2, n.logical_type);
                            detail::run(scan.get());
                            ++count;
                        }
                    }
                    sql(connection, "CLOSE polygraph_identity_scan");
                }
                snapshot.commit();
                const auto rows = detail::statement(db.get(), "SELECT key,type FROM scan ORDER BY key COLLATE BINARY");
                const auto lookup = detail::statement(db.get(), "SELECT active FROM identities WHERE key=?");
                const auto seen = detail::statement(db.get(), "UPDATE identities SET seen=1 WHERE key=?");
                int status;
                while ((status = sqlite3_step(rows.get())) == SQLITE_ROW) {
                    const auto key = detail::text(rows.get(), 0);
                    detail::bind(lookup.get(), 1, key);
                    const auto found = sqlite3_step(lookup.get());
                    if (found == SQLITE_ROW) {
                        if (sqlite3_column_int(lookup.get(), 0) != 1)
                            detail::fail("retired application key reappeared; explicit identity migration required");
                        detail::bind(seen.get(), 1, key);
                        detail::run(seen.get());
                    } else if (found == SQLITE_DONE) {
                        if (next > insertion::BackendEncodedId::payload_mask())
                            detail::fail("48-bit vertex ID payload space exhausted");
                        const auto id = insertion::BackendEncodedId::from_local_id(next++, POSTGRESQL, detail::routing);
                        insert_identity(insert.get(), id, key, detail::text(rows.get(), 1), "PostgreSQL");
                    } else
                        detail::fail("catalog identity lookup failed");
                    sqlite3_reset(lookup.get());
                    sqlite3_clear_bindings(lookup.get());
                }
                if (status != SQLITE_DONE)
                    detail::fail("catalog scan failed");
                detail::exec(db.get(), "UPDATE identities SET active=0 WHERE seen=0; DROP TABLE scan;");
                metadata(db.get(), "next_payload", std::to_string(next));
                detail::exec(db.get(), "COMMIT");
            }
            const auto hash = detail::hash_file(index);
            json manifest{
                    {"format", "polygraph.relational-identity"},
                    {"version", 1},
                    {"schema_version", 1},
                    {"engine_version", 1},
                    {"state", "prepared"},
                    {"id_strategy", "typed-external-v1"},
                    {"generation_id", hash},
                    {"mapping_sha256", fingerprint},
                    {"index",
                     {{"path", "catalog.sqlite3"}, {"bytes", std::filesystem::file_size(index)}, {"sha256", hash}}},
                    {"counts", {{"nodes", count}}}};
            if (previous)
                manifest["previous_generation"] = previous->manifest.at("generation_id");
            if (source)
                manifest["source_generation"] = source->manifest.at("generation_id");
            const auto temporary = root / "manifest.json.tmp";
            {
                std::ofstream out(temporary);
                out << manifest.dump(2) << '\n';
                out.flush();
                if (!out)
                    detail::fail("cannot write catalog manifest");
            }
            sync_path(index);
            sync_path(temporary);
            std::filesystem::rename(temporary, root / "manifest.json");
            sync_path(root);
            sync_path(root.parent_path());
            return root / "manifest.json";
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
            throw;
        }
    }
} // namespace polygraph::postgres
