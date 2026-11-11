#pragma once

#include <array>
#include <climits>
#include <fstream>
#include <memory>
#include <sqlite3.h>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/uuid.hpp"

namespace polygraph::postgres::detail {
    inline constexpr std::array<BackendId, 3> routing{NEO4J, ARANGO, POSTGRESQL};
    [[noreturn]] inline void fail(const std::string &message) {
        throw std::invalid_argument("Relational identity: " + message);
    }
    inline BackendId catalog_owner(const std::string &name) {
        if (name == "Neo4J" || name == "Neo4j" || name == "NEO4J")
            return NEO4J;
        if (name == "ArangoDB" || name == "Arango" || name == "ARANGO")
            return ARANGO;
        if (name == "PostgreSQL" || name == "Postgres" || name == "POSTGRESQL")
            return POSTGRESQL;
        fail("Unsupported catalog owner '" + name + "'");
    }
    using Db = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    inline Db open(const std::filesystem::path &path, bool readonly) {
        sqlite3 *raw = nullptr;
        const int flags = readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        const auto status = sqlite3_open_v2(path.c_str(), &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
        Db db(raw, sqlite3_close);
        if (status != SQLITE_OK)
            fail("cannot open catalog '" + path.string() + "'");
        return db;
    }
    inline void exec(sqlite3 *db, const char *sql) {
        char *raw = nullptr;
        const auto status = sqlite3_exec(db, sql, nullptr, nullptr, &raw);
        const std::unique_ptr<char, decltype(&sqlite3_free)> message(raw, sqlite3_free);
        if (status != SQLITE_OK)
            fail("catalog operation failed: " + std::string(raw ? raw : sqlite3_errmsg(db)));
    }
    inline Statement statement(sqlite3 *db, const char *sql) {
        sqlite3_stmt *raw = nullptr;
        const auto status = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
        Statement result(raw, sqlite3_finalize);
        if (status != SQLITE_OK)
            fail("invalid catalog schema: " + std::string(sqlite3_errmsg(db)));
        return result;
    }
    inline void bind(sqlite3_stmt *s, int col, const std::string &value) {
        if (value.size() > INT_MAX ||
            sqlite3_bind_text(s, col, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            fail("catalog text binding failed");
    }
    inline void bind(sqlite3_stmt *s, int col, std::int64_t value) {
        if (sqlite3_bind_int64(s, col, value) != SQLITE_OK)
            fail("catalog integer binding failed");
    }
    inline void run(sqlite3_stmt *s) {
        if (sqlite3_step(s) != SQLITE_DONE)
            fail("duplicate identity/ID or catalog write failure");
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    inline std::string text(sqlite3_stmt *s, int col) {
        if (sqlite3_column_type(s, col) != SQLITE_TEXT)
            fail("invalid catalog text field");
        return {reinterpret_cast<const char *>(sqlite3_column_text(s, col)),
                static_cast<std::size_t>(sqlite3_column_bytes(s, col))};
    }
    inline GlobalId id(sqlite3_stmt *s, int col) {
        if (sqlite3_column_type(s, col) != SQLITE_INTEGER)
            fail("invalid catalog ID field");
        return global_id_from_signed_storage(sqlite3_column_int64(s, col));
    }
    [[nodiscard]] std::string hash_file(const std::filesystem::path &);
    [[nodiscard]] std::string hash_text(std::string_view);
    struct CatalogFile {
        json manifest;
        std::filesystem::path index;
    };
    [[nodiscard]] CatalogFile read_catalog_file(const std::filesystem::path &, bool relational);
    [[nodiscard]] inline const NodeMapping &node_by_name(const RelationalMapping &mapping, const std::string &name) {
        for (const auto &n: mapping.nodes())
            if (n.logical_type == name)
                return n;
        fail("unknown mapped logical type '" + name + "'");
    }
    [[nodiscard]] inline domain::SourceIdentity decode_key(const std::string &key) {
        const auto v = json::parse(key);
        if (!v.is_array() || v.size() != 6 || v[0] != "polygraph.source.v1" || v[2] != "vertex")
            fail("invalid typed source key");
        return domain::source_identity_from_json({{"namespace", v[1]}, {"type", v[3]}, {"idType", v[4]}, {"id", v[5]}});
    }
} // namespace polygraph::postgres::detail
