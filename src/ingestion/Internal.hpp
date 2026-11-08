#ifndef POLYGRAPH_INGESTION_INTERNAL_HPP
#define POLYGRAPH_INGESTION_INTERNAL_HPP
#include <chrono>
#include <fstream>
#include <span>
#include <sqlite3.h>
#include "ingestion/Import.hpp"
#include "ingestion/InputAdapterRegistry.hpp"

namespace polygraph::ingestion::detail {
    inline const InputAdapterRegistry &input_adapters(const PrepareOptions &options) {
        return options.input_adapters ? *options.input_adapters : default_input_adapters();
    }
    void check_prepare_options(const PrepareOptions &options);
    [[nodiscard]] Json checked_targets(const Json &config, std::span<const Backend> backends);
    [[nodiscard]] unsigned arango_import_tuning(const Json &target, const std::string &name, unsigned fallback,
                                                unsigned maximum);
    void validate_arango_import_options(const Json &target);
    using Clock = std::chrono::steady_clock;
    inline double elapsed(Clock::time_point start) {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
    class Statement {
    public:
        Statement(sqlite3 *db, const std::string &sql);
        ~Statement();
        Statement(const Statement &) = delete;
        Statement &operator=(const Statement &) = delete;
        void bind(int column, const std::string &value);
        void bind(int column, std::int64_t value);
        [[nodiscard]] bool step();
        void run();
        void reset();
        [[nodiscard]] std::string text(int column) const;
        [[nodiscard]] std::int64_t integer(int column) const;

    private:
        sqlite3 *db_;
        sqlite3_stmt *stmt_ = nullptr;
    };
    class Database {
    public:
        Database(const fs::path &path, unsigned cache_mb, bool readonly = false);
        ~Database();
        Database(const Database &) = delete;
        Database &operator=(const Database &) = delete;
        void exec(const std::string &sql);
        [[nodiscard]] Statement statement(const std::string &sql) { return Statement(db_, sql); }

    private:
        sqlite3 *db_ = nullptr;
    };
    class DirectoryLock {
    public:
        explicit DirectoryLock(const fs::path &directory);
        ~DirectoryLock();
        DirectoryLock(const DirectoryLock &) = delete;
        DirectoryLock &operator=(const DirectoryLock &) = delete;

    private:
        int descriptor_ = -1;
    };
    class AtomicOutput {
    public:
        explicit AtomicOutput(fs::path path, std::size_t buffer_bytes = 65536);
        ~AtomicOutput();
        AtomicOutput(const AtomicOutput &) = delete;
        AtomicOutput &operator=(const AtomicOutput &) = delete;
        void write(const std::string &text);
        void commit();

    private:
        fs::path path_;
        fs::path temporary_;
        std::vector<char> buffer_;
        std::ofstream stream_;
        bool committed_ = false;
    };
    [[nodiscard]] std::unique_ptr<IGraphInputAdapter> make_adapter(const PrepareOptions &options,
                                                                   const fs::path &scratch);
    [[nodiscard]] Json normalize(const PrepareOptions &options, const Placement &placement, const fs::path &scratch);
    [[nodiscard]] bool partition_metadata_enabled(const PrepareOptions &options) noexcept;
    [[nodiscard]] Json load_partition_mapping(const PrepareOptions &options, const Placement &placement);
    [[nodiscard]] Json prepare_partition_assignments(const PrepareOptions &options, Database &db,
                                                     const fs::path &scratch);
    void finish_partition_metadata(const PrepareOptions &options, Database &db, const fs::path &scratch,
                                   Json &partitioning);
    void verify_partition_metadata(Database &db, const fs::path &root, const Json &manifest);
    [[nodiscard]] Json generate_backend(Backend backend, const PrepareOptions &options, const fs::path &scratch,
                                        const fs::path &generation, const fs::path &root,
                                        bool partition_metadata = false);
    [[nodiscard]] fs::path artifact_path(const fs::path &root, const Json &artifact);
    [[nodiscard]] Json input_identity(const PrepareOptions &options);
    [[nodiscard]] std::vector<fs::path> input_paths(const PrepareOptions &options);
    [[nodiscard]] std::string csv_row(const std::vector<std::string> &fields);
    [[nodiscard]] bool read_csv(std::istream &stream, std::vector<std::string> &fields, char delimiter,
                                std::size_t max_bytes);
    void validate_properties(const Json &properties);
    void verify_semantics(const fs::path &root, const Json &manifest);
} // namespace polygraph::ingestion::detail
#endif
