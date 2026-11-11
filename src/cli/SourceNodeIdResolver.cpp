#include "cli/SourceNodeIdResolver.hpp"
#include <climits>
#include <fstream>
#include <memory>
#include <sqlite3.h>
#include "cli/BackendConfigParser.hpp"
#include "utilities/uuid.hpp"

namespace polygraph::cli {
    SourceNodeIdResolver::SourceNodeIdResolver(const std::filesystem::path &manifest_path) {
        const auto root = std::filesystem::canonical(manifest_path).parent_path();
        std::ifstream input(manifest_path);
        if (!input)
            throw std::runtime_error("Could not open prepared source index manifest: " + manifest_path.string());
        const auto manifest = json::parse(input);
        if (manifest.at("schema_version") != 1 || manifest.at("engine_version") != 1 ||
            manifest.at("state") != "prepared" || manifest.at("id_strategy") != "typed-external-v1")
            throw std::runtime_error("Source roots require a prepared typed-external-v1 index: " +
                                     manifest_path.string());
        const std::filesystem::path relative = manifest.at("index").at("path").get<std::string>();
        if (relative.empty() || relative.is_absolute())
            throw std::runtime_error("Source index path must be relative to prepared artifacts");
        index_ = std::filesystem::canonical(root / relative);
        auto r = root.begin(), p = index_.begin();
        for (; r != root.end() && p != index_.end() && *r == *p; ++r, ++p) {
        }
        if (r != root.end() || index_ == root || !std::filesystem::is_regular_file(index_))
            throw std::runtime_error("Source index escapes prepared artifacts");
        if (std::filesystem::file_size(index_) != manifest.at("index").at("bytes").get<std::uintmax_t>())
            throw std::runtime_error("Source index size differs from prepared manifest; verify/reprepare artifacts");
    }
    ResolvedSourceNode SourceNodeIdResolver::resolve(const domain::SourceIdentity &identity) const {
        const auto key = domain::encode_source_identity(identity);
        sqlite3 *raw = nullptr;
        const auto status =
                sqlite3_open_v2(index_.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        if (status != SQLITE_OK)
            throw std::runtime_error("Cannot open prepared source index read-only: " + index_.string());
        sqlite3_stmt *statement = nullptr;
        const auto prepared =
                sqlite3_prepare_v2(db.get(), "SELECT id,type,owner FROM nodes WHERE key=?", -1, &statement, nullptr);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
        if (prepared != SQLITE_OK || key.size() > INT_MAX ||
            sqlite3_bind_text(query.get(), 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("Invalid prepared source index lookup: " + index_.string());
        const auto step = sqlite3_step(query.get());
        if (step == SQLITE_DONE)
            throw std::runtime_error("Unknown vertex source identity: " + key);
        if (step != SQLITE_ROW || sqlite3_column_type(query.get(), 0) != SQLITE_INTEGER ||
            sqlite3_column_type(query.get(), 1) != SQLITE_TEXT || sqlite3_column_type(query.get(), 2) != SQLITE_TEXT)
            throw std::runtime_error("Invalid prepared vertex source mapping: " + key);
        const std::string type = reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 1));
        const std::string owner = reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 2));
        if (type != identity.type)
            throw std::runtime_error("Prepared source mapping type disagrees with identity: " + key);
        const ResolvedSourceNode result{global_id_from_signed_storage(sqlite3_column_int64(query.get(), 0)),
                                        config::backend_id_from_string(owner)};
        if (sqlite3_step(query.get()) != SQLITE_DONE)
            throw std::runtime_error("Duplicate/error in prepared source index: " + key);
        return result;
    }
} // namespace polygraph::cli
