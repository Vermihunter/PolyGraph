#include <atomic>
#include <mutex>
#include <set>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include "Internal.hpp"

namespace polygraph::ingestion {
    namespace {
        void check_options(const PrepareOptions &o) {
            if (o.partitioner != "none" && o.partitioner != "kaminpar")
                throw Error("--partitioner must be none or kaminpar");
            if (o.partitioner == "kaminpar") {
                if (!kaminpar_available())
                    throw Error("KaMinPar is unavailable; build with POLYGRAPH_INGESTION_KAMINPAR=ON");
                validate_partition_options(o.partition_options, o.seed);
                if (!o.partition_file.empty())
                    throw Error("Generated KaMinPar partitions and --partition-file are mutually exclusive");
            } else if (o.partition_options.count != 0 || o.partition_options.threads != 1 ||
                       o.partition_options.imbalance != 0.03 || o.partition_options.preset != "default")
                throw Error("Partition count/threads/imbalance/preset require --partitioner kaminpar");
            if (o.threads < 1 || o.threads > 256)
                throw Error("--threads must be in [1,256]");
            if (o.memory_limit_mb < 16 || o.memory_limit_mb > 1048576)
                throw Error("--memory-limit-mb must be in [16,1048576]");
            if (o.chunk_size_mb < 1 || o.chunk_size_mb > 4096)
                throw Error("--chunk-size-mb must be in [1,4096]");
            if (o.max_record_mb < 1 || o.max_record_mb > o.memory_limit_mb / 4)
                throw Error("--max-record-mb must fit within one quarter of --memory-limit-mb");
            if (o.input.empty() || o.output_dir.empty() || o.placement_config.empty())
                throw Error("prepare requires --input, --output-dir and --placement-config");
            const auto input = fs::weakly_canonical(o.input), output = fs::weakly_canonical(o.output_dir);
            if (output == input || (fs::is_directory(input) && !output.lexically_relative(input).empty() &&
                                    *output.lexically_relative(input).begin() != ".."))
                throw Error("Artifact directory must be outside the input dataset");
        }
        Json configuration(const PrepareOptions &o, const Placement &p) {
            Json j = {{"engine_version", 1}, {"placement", p.configuration},     {"partitioner", o.partitioner},
                      {"seed", o.seed},      {"chunk_size_mb", o.chunk_size_mb}, {"max_record_mb", o.max_record_mb}};
            if (!o.partition_file.empty())
                j["partition_file_sha256"] = sha256_file(o.partition_file);
            if (!o.partition_map.empty())
                j["partition_mapping"] = read_json(o.partition_map);
            if (detail::partition_metadata_enabled(o))
                j["partition_metadata_version"] = 1;
            if (o.partitioner == "kaminpar")
                j["kaminpar"] = {{"partitions", o.partition_options.count},
                                 {"threads", o.partition_options.threads},
                                 {"imbalance", o.partition_options.imbalance},
                                 {"preset", o.partition_options.preset},
                                 {"implementation", kaminpar_identity()}};
            return j;
        }
        void check_backend_artifacts(const fs::path &root, const Json &backend) {
            for (const auto &artifact: backend.at("artifacts")) {
                const auto path = detail::artifact_path(root, artifact);
                if (fs::file_size(path) != artifact.at("bytes").get<std::uint64_t>() ||
                    sha256_file(path) != artifact.at("sha256"))
                    throw Error("Artifact checksum/size mismatch: " + path.string());
            }
        }
    } // namespace
    Json prepare(const PrepareOptions &o, const ProgressObserver &progress) {
        const auto &adapter_definition = detail::input_adapters(o).get(o.adapter);
        check_options(o);
        const auto started = detail::Clock::now();
        detail::DirectoryLock lock(o.output_dir);
        const auto placement = load_placement(o.placement_config);
        (void) detail::load_partition_mapping(o, placement);
        const auto input = detail::input_identity(o);
        const auto config = configuration(o, placement);
        const auto identity = sha256_text(Json{{"input", input}, {"config", config}}.dump());
        const auto manifest_path = o.output_dir / "manifest.json";
        if (fs::exists(manifest_path)) {
            const auto previous = read_json(manifest_path);
            if (previous.at("identity_sha256") != identity)
                throw Error("Existing artifacts belong to different inputs/configuration; use a new --output-dir");
            return verify(manifest_path, true);
        }
        const auto generation = o.output_dir / ("generation-" + identity.substr(0, 24));
        const auto scratch = generation / "index";
        const auto stage_path = o.output_dir / "generation-state.json";
        Json stage = {{"identity_sha256", identity}, {"normalized", false}, {"backends", Json::object()}};
        if (fs::exists(stage_path)) {
            stage = read_json(stage_path);
            if (stage.at("identity_sha256") != identity)
                throw Error("Interrupted preparation has different input/configuration; use a new output directory");
        }
        if (stage.value("invalidated", false))
            throw Error("Checkpoint was invalidated by input/configuration mutation; use a fresh output directory");
        const auto validate_identity = [&] {
            try {
                if (detail::input_identity(o) == input &&
                    configuration(o, load_placement(o.placement_config)) == config)
                    return;
            } catch (...) {
                stage["invalidated"] = true;
                write_json_atomic(stage_path, stage);
                throw;
            }
            stage["invalidated"] = true;
            write_json_atomic(stage_path, stage);
            throw Error("Input/configuration changed while preparing; checkpoint invalidated. Use a new output "
                        "directory and immutable inputs");
        };
        if (progress)
            progress("source-hashed");
        Json normalized;
        if (stage.value("normalized", false)) {
            if (sha256_file(scratch / "graph.sqlite3") != stage.at("index_sha256"))
                throw Error("Normalization checkpoint is corrupt; use a fresh output directory");
            normalized = stage.at("normalization");
            if (config.contains("partition_metadata_version"))
                check_backend_artifacts(
                        o.output_dir,
                        {{"artifacts", Json::array({normalized.at("partitioning").at("assignment_artifact")})}});
        } else {
            fs::remove_all(generation);
            fs::create_directories(scratch);
            normalized = detail::normalize(o, placement, scratch);
            validate_identity();
            stage["normalized"] = true;
            stage["normalization"] = normalized;
            stage["index_sha256"] = sha256_file(scratch / "graph.sqlite3");
            fs::remove(scratch / "aux.sqlite3");
            write_json_atomic(stage_path, stage);
        }
        if (progress)
            progress("normalized");
        // Each worker owns a backend, read connection and output files. No serialization lock.
        for (const auto &[name, backend]: stage["backends"].items()) {
            (void) name;
            check_backend_artifacts(o.output_dir, backend);
        }
        std::vector<Backend> pending;
        for (const auto backend: placement.backends)
            if (!stage["backends"].contains(backend_name(backend)))
                pending.push_back(backend);
        std::atomic<std::size_t> next{0};
        std::mutex results_mutex; // protects stage and first_error only; never guards record serialization.
        std::exception_ptr first_error;
        const auto worker = [&] {
            for (;;) {
                const auto index = next.fetch_add(1);
                if (index >= pending.size())
                    return;
                const auto backend = pending[index];
                try {
                    fs::remove_all(generation / backend_name(backend));
                    auto result = detail::generate_backend(backend, o, scratch, generation, o.output_dir,
                                                           detail::partition_metadata_enabled(o));
                    std::scoped_lock guard(results_mutex);
                    stage["backends"][backend_name(backend)] = std::move(result);
                    write_json_atomic(stage_path, stage);
                } catch (...) {
                    std::scoped_lock guard(results_mutex);
                    if (!first_error)
                        first_error = std::current_exception();
                }
            }
        };
        {
            std::vector<std::jthread> workers;
            for (unsigned i = 0; i < std::min<std::size_t>(o.threads, pending.size()); ++i)
                workers.emplace_back(worker);
        }
        if (first_error)
            std::rethrow_exception(first_error);
        validate_identity();
        if (progress)
            progress("artifacts-ready");
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        Json manifest = {{"schema_version", 1},
                         {"engine_version", 1},
                         {"generation_id", identity.substr(0, 24)},
                         {"identity_sha256", identity},
                         {"source", input},
                         {"configuration", config},
                         {"configuration_sha256", sha256_text(config.dump())},
                         {"id_strategy", adapter_definition.id_policy == InputIdPolicy::ldbc_block31
                                                 ? "ldbc-block31-v3"
                                                 : "preserve-canonical-v2"},
                         {"counts", normalized.at("counts")},
                         {"metadata", normalized.at("metadata")},
                         {"node_prefixes", normalized.at("node_prefixes")},
                         {"partitioning", normalized.at("partitioning")},
                         {"timings", normalized.at("timings")},
                         {"backends", stage.at("backends")},
                         {"index",
                          {{"path", fs::relative(scratch / "graph.sqlite3", o.output_dir).generic_string()},
                           {"bytes", fs::file_size(scratch / "graph.sqlite3")},
                           {"sha256", stage.at("index_sha256")}}},
                         {"state", "prepared"},
                         {"limits",
                          {{"memory_limit_mb", o.memory_limit_mb},
                           {"threads", o.threads},
                           {"chunk_size_mb", o.chunk_size_mb},
                           {"max_record_mb", o.max_record_mb}}}};
        manifest["timings"]["prepare_seconds"] = detail::elapsed(started);
        manifest["timings"]["input_nodes_per_second"] =
                normalized["counts"]["nodes"].get<double>() / normalized["timings"]["nodes_seconds"].get<double>();
        manifest["timings"]["input_edges_per_second"] =
                normalized["counts"]["edges"].get<double>() / normalized["timings"]["edges_seconds"].get<double>();
        manifest["peak_rss_kb"] = usage.ru_maxrss;
        // Publish only after closed endpoints, all writers, and checksum validation have succeeded.
        for (const auto &backend: manifest["backends"])
            check_backend_artifacts(o.output_dir, backend);
        if (config.contains("partition_metadata_version"))
            check_backend_artifacts(
                    o.output_dir,
                    {{"artifacts", Json::array({normalized.at("partitioning").at("assignment_artifact")})}});
        write_json_atomic(manifest_path, manifest);
        return manifest;
    }
    Json verify(const fs::path &manifest_path, bool strict) {
        const auto m = read_json(manifest_path);
        if (m.at("schema_version") != 1 || m.at("engine_version") != 1 || m.at("state") != "prepared")
            throw Error("Unsupported or incomplete ingestion manifest");
        if (sha256_text(m.at("configuration").dump()) != m.at("configuration_sha256"))
            throw Error("Configuration checksum mismatch");
        if (sha256_text(Json{{"input", m.at("source")}, {"config", m.at("configuration")}}.dump()) !=
            m.at("identity_sha256"))
            throw Error("Input/configuration identity mismatch");
        const auto root = fs::absolute(manifest_path).parent_path();
        std::set<Backend> required_backends;
        for (const auto &name: m.at("configuration").at("placement").at("storageBackends"))
            required_backends.insert(parse_backend(name.get<std::string>()));
        std::set<Backend> present_backends;
        for (const auto &[name, value]: m.at("backends").items()) {
            (void) value;
            present_backends.insert(parse_backend(name));
        }
        if (required_backends != present_backends)
            throw Error("Manifest is missing a required storage backend");
        std::set<fs::path> paths;
        for (const auto &[name, backend]: m.at("backends").items()) {
            (void) parse_backend(name);
            if (backend.at("state") != "prepared")
                throw Error("Incomplete backend artifacts: " + name);
            check_backend_artifacts(root, backend);
            for (const auto &artifact: backend.at("artifacts")) {
                const auto path = detail::artifact_path(root, artifact);
                if (!paths.insert(path).second)
                    throw Error("Duplicate artifact path");
            }
        }
        const auto index = detail::artifact_path(root, m.at("index"));
        if (fs::file_size(index) != m["index"]["bytes"].get<std::uint64_t>() ||
            sha256_file(index) != m["index"]["sha256"])
            throw Error("Endpoint index checksum mismatch");
        if (m.at("configuration").contains("partition_metadata_version")) {
            if (m["configuration"]["partition_metadata_version"] != 1)
                throw Error("Unsupported partition metadata version");
            const auto &assignment = m.at("partitioning").at("assignment_artifact");
            check_backend_artifacts(root, {{"artifacts", Json::array({assignment})}});
            if (!paths.insert(detail::artifact_path(root, assignment)).second)
                throw Error("Duplicate partition assignment artifact path");
        }
        if (strict)
            detail::verify_semantics(root, m);
        return m;
    }
} // namespace polygraph::ingestion
namespace polygraph::ingestion::detail {
    void verify_semantics(const fs::path &root, const Json &manifest) {
        const auto scratch = artifact_path(root, manifest.at("index")).parent_path();
        Database db(scratch / "graph.sqlite3", 4, true);
        auto integrity = db.statement("PRAGMA integrity_check");
        if (!integrity.step() || integrity.text(0) != "ok")
            throw Error("SQLite endpoint index failed integrity check");
        auto missing = db.statement("SELECT e.id FROM edges e LEFT JOIN nodes f ON f.id=e.from_id LEFT JOIN nodes t ON "
                                    "t.id=e.to_id WHERE f.id IS NULL OR t.id IS NULL LIMIT 1");
        if (missing.step())
            throw Error("Unresolved endpoint in normalized index");
        verify_partition_metadata(db, root, manifest);
        for (const auto &[table, label]:
             std::array<std::pair<std::string, std::string>, 2>{{{"nodes", "nodes"}, {"edges", "edges"}}}) {
            auto count = db.statement("SELECT count(*) FROM " + table);
            if (!count.step() || count.integer(0) != manifest.at("counts").at(label).get<std::int64_t>())
                throw Error("Manifest entity count mismatch");
        }
        PrepareOptions o;
        o.memory_limit_mb = 16;
        o.threads = 1;
        o.chunk_size_mb = manifest.at("limits").at("chunk_size_mb").get<unsigned>();
        const auto tmp = fs::temp_directory_path() / ("polygraph-verify-" + std::to_string(getpid()) + "-" +
                                                      sha256_text(root.string()).substr(0, 16));
        DirectoryLock lock(tmp);
        struct Cleanup {
            fs::path path;
            ~Cleanup() {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } cleanup{tmp};
        for (const auto &[name, expected]: manifest.at("backends").items()) {
            const auto actual = generate_backend(parse_backend(name), o, scratch, tmp, tmp,
                                                 manifest.at("configuration").contains("partition_metadata_version"));
            if (actual.at("counts") != expected.at("counts") ||
                actual.at("artifacts").size() != expected.at("artifacts").size())
                throw Error("Native artifact counts disagree with endpoint index: " + name);
            for (std::size_t i = 0; i < actual["artifacts"].size(); ++i) {
                const auto &a = actual["artifacts"][i], &e = expected["artifacts"][i];
                for (const auto *key: {"kind", "collection", "records", "bytes", "sha256"})
                    if (a.at(key) != e.at(key))
                        throw Error("Native representation differs from normalized records: " +
                                    e.at("path").get<std::string>());
            }
        }
    }
} // namespace polygraph::ingestion::detail
