#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>
#include "Internal.hpp"

namespace polygraph::ingestion {
    namespace {
        using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
        std::string digest(std::istream &input) {
            Digest ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
            if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
                throw Error("Cannot initialize SHA-256");
            std::array<char, 65536> buffer{};
            while (input) {
                input.read(buffer.data(), buffer.size());
                if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(input.gcount())) != 1)
                    throw Error("SHA-256 failed");
            }
            if (!input.eof())
                throw Error("Read failed while hashing");
            std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
            unsigned length = 0;
            if (EVP_DigestFinal_ex(ctx.get(), bytes.data(), &length) != 1)
                throw Error("SHA-256 finalization failed");
            std::ostringstream out;
            for (unsigned i = 0; i < length; ++i)
                out << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(bytes[i]);
            return out.str();
        }
    } // namespace
    std::string sha256_file(const fs::path &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw Error("Cannot open for checksum: " + path.string());
        return digest(in);
    }
    std::string sha256_text(const std::string &text) {
        std::istringstream in(text);
        return digest(in);
    }
    Json read_json(const fs::path &path) {
        std::ifstream in(path);
        if (!in)
            throw Error("Cannot read JSON: " + path.string());
        try {
            Json j;
            in >> j;
            in >> std::ws;
            if (!in.eof())
                throw Error("Trailing data in " + path.string());
            return j;
        } catch (const Json::exception &e) {
            throw Error(path.string() + ": " + e.what());
        }
    }
    void write_json_atomic(const fs::path &path, const Json &value) {
        detail::AtomicOutput out(path);
        out.write(value.dump(2) + "\n");
        out.commit();
    }
} // namespace polygraph::ingestion
namespace polygraph::ingestion::detail {
    Statement::Statement(sqlite3 *db, const std::string &sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            throw Error("SQLite prepare: " + std::string(sqlite3_errmsg(db_)));
    }
    Statement::~Statement() { sqlite3_finalize(stmt_); }
    void Statement::bind(int column, const std::string &value) {
        if (value.size() > static_cast<std::size_t>(INT_MAX) ||
            sqlite3_bind_text(stmt_, column, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
                    SQLITE_OK)
            throw Error("SQLite bind text failed");
    }
    void Statement::bind(int column, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, column, value) != SQLITE_OK)
            throw Error("SQLite bind integer failed");
    }
    bool Statement::step() {
        const auto result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        throw Error("SQLite (duplicate/colliding ID or I/O error): " + std::string(sqlite3_errmsg(db_)));
    }
    void Statement::run() {
        if (step())
            throw Error("Unexpected SQLite row");
        reset();
    }
    void Statement::reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    std::string Statement::text(int column) const {
        const auto *value = sqlite3_column_text(stmt_, column);
        return value ? std::string(reinterpret_cast<const char *>(value),
                                   static_cast<std::size_t>(sqlite3_column_bytes(stmt_, column)))
                     : "";
    }
    std::int64_t Statement::integer(int column) const { return sqlite3_column_int64(stmt_, column); }
    Database::Database(const fs::path &path, unsigned cache_mb, bool readonly) {
        const auto flags = readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            const auto message = db_ ? std::string(sqlite3_errmsg(db_)) : "allocation failed";
            sqlite3_close(db_);
            db_ = nullptr;
            throw Error("SQLite open: " + message);
        }
        try {
            exec("PRAGMA cache_size=-" + std::to_string(static_cast<std::uint64_t>(cache_mb) * 1024));
            exec("PRAGMA mmap_size=0");
            exec("PRAGMA temp_store=FILE");
            if (!readonly) {
                exec("PRAGMA journal_mode=DELETE");
                exec("PRAGMA synchronous=FULL");
            }
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }
    Database::~Database() { sqlite3_close(db_); }
    void Database::exec(const std::string &sql) {
        char *error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "unknown SQLite error";
            sqlite3_free(error);
            throw Error(message);
        }
    }
    DirectoryLock::DirectoryLock(const fs::path &directory) {
        fs::create_directories(directory);
        descriptor_ = ::open((directory / ".ingestion.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor_ < 0)
            throw Error("Cannot create ingestion lock: " + std::string(std::strerror(errno)));
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw Error("Another ingestion process holds this artifact directory");
        }
    }
    DirectoryLock::~DirectoryLock() {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }
    AtomicOutput::AtomicOutput(fs::path path, std::size_t buffer_bytes) :
        path_(std::move(path)), temporary_(path_.string() + ".tmp"), buffer_(buffer_bytes) {
        fs::create_directories(path_.parent_path());
        stream_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        stream_.exceptions(std::ios::badbit | std::ios::failbit);
        stream_.open(temporary_, std::ios::binary | std::ios::trunc);
    }
    AtomicOutput::~AtomicOutput() {
        if (!committed_) {
            stream_.exceptions(std::ios::goodbit);
            stream_.close();
            std::error_code ignored;
            fs::remove(temporary_, ignored);
        }
    }
    void AtomicOutput::write(const std::string &text) {
        stream_.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    void AtomicOutput::commit() {
        stream_.flush();
        stream_.close();
        const int fd = ::open(temporary_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            throw Error("Cannot open artifact for fsync");
        const auto result = ::fsync(fd);
        ::close(fd);
        if (result != 0)
            throw Error("Cannot fsync artifact: " + temporary_.string());
        fs::rename(temporary_, path_);
        const int directory = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            throw Error("Cannot open artifact directory for fsync");
        const auto synced = ::fsync(directory);
        ::close(directory);
        if (synced != 0)
            throw Error("Cannot fsync artifact directory");
        committed_ = true;
    }
    fs::path artifact_path(const fs::path &root, const Json &artifact) {
        const fs::path relative = artifact.at("path").get<std::string>();
        if (relative.empty() || relative.is_absolute())
            throw Error("Artifact path must be relative");
        for (const auto &component: relative)
            if (component == "..")
                throw Error("Artifact path escapes manifest directory");
        const auto base = fs::canonical(root);
        const auto actual = fs::weakly_canonical(root / relative);
        const auto within = actual.lexically_relative(base);
        if (within.empty() || *within.begin() == "..")
            throw Error("Artifact symlink escapes manifest directory");
        if (!fs::is_regular_file(actual))
            throw Error("Missing artifact: " + actual.string());
        return actual;
    }
    std::string csv_row(const std::vector<std::string> &fields) {
        std::string line;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i)
                line += ',';
            line += '"';
            for (const char c: fields[i]) {
                line += c;
                if (c == '"')
                    line += '"';
            }
            line += '"';
        }
        return line + '\n';
    }
    bool read_csv(std::istream &in, std::vector<std::string> &fields, char delimiter, std::size_t max_bytes) {
        fields.clear();
        std::string field;
        bool quoted = false, closed = false, started = false, any = false;
        std::size_t bytes = 0;
        char c = 0;
        while (in.get(c)) {
            any = true;
            if (++bytes > max_bytes)
                throw Error("CSV record exceeds configured --max-record-mb");
            if (c == '\0')
                throw Error("NUL in CSV record");
            if (quoted) {
                if (c == '"') {
                    if (in.peek() == '"') {
                        in.get(c);
                        ++bytes;
                        field += '"';
                    } else {
                        quoted = false;
                        closed = true;
                    }
                } else
                    field += c;
            } else if (c == delimiter) {
                fields.push_back(std::move(field));
                field.clear();
                started = false;
                closed = false;
            } else if (c == '\n' || c == '\r') {
                if (c == '\r' && in.peek() == '\n')
                    in.get(c);
                fields.push_back(std::move(field));
                return true;
            } else if (c == '"' && !started && field.empty() && !closed) {
                quoted = true;
                started = true;
            } else {
                if (closed || c == '"')
                    throw Error("Invalid quoting in CSV record");
                started = true;
                field += c;
            }
        }
        if (!in.eof())
            throw Error("CSV read failed");
        if (quoted)
            throw Error("Unterminated quoted CSV field");
        if (any)
            fields.push_back(std::move(field));
        return any;
    }
    void validate_properties(const Json &properties) {
        if (!properties.is_object())
            throw Error("properties must be an object");
        // JSONB and Neo4j cannot represent NUL in a string. Reject uniformly rather than silently truncating.
        const auto check = [](const auto &self, const Json &value) -> void {
            if (value.is_string() && value.get_ref<const std::string &>().find('\0') != std::string::npos)
                throw Error("NUL in property string");
            if (value.is_object())
                for (const auto &[key, item]: value.items()) {
                    if (key.find('\0') != std::string::npos)
                        throw Error("NUL in property key");
                    self(self, item);
                }
            else if (value.is_array())
                for (const auto &item: value)
                    self(self, item);
        };
        check(check, properties);
    }
} // namespace polygraph::ingestion::detail
