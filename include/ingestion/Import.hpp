#ifndef POLYGRAPH_INGESTION_IMPORT_HPP
#define POLYGRAPH_INGESTION_IMPORT_HPP

#include "ingestion/Partition.hpp"
#include "utilities/json.hpp"

#include <bit>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace polygraph::ingestion {
    using Json = nlohmann::json;
    namespace fs = std::filesystem;
    using Id = std::uint64_t;
    enum class Backend { neo4j, arango, postgresql };
    class Error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };
    [[nodiscard]] std::string backend_name(Backend backend);
    [[nodiscard]] Backend parse_backend(const std::string &name);
    [[nodiscard]] Backend owner(Id id);
    [[nodiscard]] Id canonical_id(Id id);
    [[nodiscard]] Id make_id(Backend backend, Id payload);
    [[nodiscard]] Id parse_id(const Json &value);
    [[nodiscard]] constexpr Id local_id(Id id) noexcept { return id & ((Id{1} << 48) - 1); }
    [[nodiscard]] constexpr std::int64_t signed_id(Id id) noexcept { return std::bit_cast<std::int64_t>(id); }
    [[nodiscard]] std::string sanitize_name(const std::string &name);

    struct Record {
        std::string key;
        std::string type;
        Json properties = Json::object();
        std::optional<Id> id;
        std::optional<Backend> source;
        std::string from;
        std::string to;
        std::optional<unsigned> partition;
    };
    using RecordConsumer = std::function<void(Record)>;
    // Each call streams one phase, never retaining the graph. Consumers may batch externally.
    class IGraphInputAdapter {
    public:
        virtual ~IGraphInputAdapter() = default;
        virtual void nodes(const RecordConsumer &consume) = 0;
        virtual void relationships(const RecordConsumer &consume) = 0;
        [[nodiscard]] virtual Json metadata() const = 0;
    };

    class InputAdapterRegistry;
    struct PrepareOptions {
        std::string adapter;
        fs::path input;
        fs::path placement_config;
        fs::path output_dir;
        std::string partitioner = "none";
        PartitionOptions partition_options;
        fs::path partition_file;
        fs::path partition_map;
        unsigned seed = 42;
        unsigned threads = 3;
        unsigned memory_limit_mb = 256;
        unsigned chunk_size_mb = 64;
        unsigned max_record_mb = 8;
        // Optional caller-owned registry; must outlive prepare(). Null selects immutable built-ins.
        const InputAdapterRegistry *input_adapters = nullptr;
    };
    struct Placement {
        Json configuration;
        std::vector<Backend> backends;
        [[nodiscard]] Backend route(const Record &record, bool edge) const;
        void validate(Backend backend) const;
    };
    [[nodiscard]] Placement load_placement(const fs::path &path);
    [[nodiscard]] std::string sha256_file(const fs::path &path);
    [[nodiscard]] std::string sha256_text(const std::string &text);
    [[nodiscard]] Json read_json(const fs::path &path);
    void write_json_atomic(const fs::path &path, const Json &value);
    using ProgressObserver = std::function<void(const std::string &stage)>;
    [[nodiscard]] Json prepare(const PrepareOptions &options, const ProgressObserver &progress = {});
    // Always validates checksums and sizes. Strict also checks native record semantics and endpoint closure.
    [[nodiscard]] Json verify(const fs::path &manifest, bool strict = false);
    enum class VerificationMode { full, none };
    struct ImportContext {
        Backend backend;
        fs::path root;
        Json artifacts;
        Json target;
        VerificationMode verification = VerificationMode::full;
    };
    class IImportExecutor {
    public:
        virtual ~IImportExecutor() = default;
        virtual void preflight() = 0;
        [[nodiscard]] virtual Json run() = 0;
        [[nodiscard]] virtual Json verify() = 0;
    };
    using ExecutorFactory = std::function<std::unique_ptr<IImportExecutor>(ImportContext)>;
    void execute(const fs::path &manifest, const fs::path &targets, bool confirm_create, unsigned concurrency = 3,
                 const ExecutorFactory &factory = {}, VerificationMode verification = VerificationMode::full);
    [[nodiscard]] Json verify_live(const fs::path &manifest, const fs::path &targets,
                                   const ExecutorFactory &factory = {});
} // namespace polygraph::ingestion
#endif
