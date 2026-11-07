#include <atomic>
#include <mutex>
#include <regex>
#include <sys/resource.h>
#include <thread>
#include "Native.hpp"

namespace polygraph::ingestion {
    namespace {
        std::vector<ImportContext> contexts(const fs::path &root, const Json &manifest, const Json &config,
                                            VerificationMode verification = VerificationMode::full) {
            if (!config.is_object() || !config.contains("targets") || !config["targets"].is_object())
                throw Error("Target configuration requires a targets object");
            std::vector<ImportContext> result;
            for (const auto &[name, artifacts]: manifest.at("backends").items()) {
                if (!config["targets"].contains(name))
                    throw Error("Missing named target for " + name);
                auto target = config["targets"][name];
                const auto database = target.at("database").get<std::string>();
                static const std::regex safe("[A-Za-z][A-Za-z0-9_.-]{2,62}");
                if (!std::regex_match(database, safe) || database == "neo4j" || database == "system" ||
                    database == "_system" || database == "postgres" || database == "template0" ||
                    database == "template1" || database == "polygraph")
                    throw Error("Use an explicit isolated named target, not a default/system database: " + database);
                if (name == "Neo4J" && !std::regex_match(database, std::regex("[a-z][a-z0-9.-]{2,62}")))
                    throw Error("Neo4j database names must use lowercase ASCII letters, digits, dots and dashes (no "
                                "underscores)");
                if (target.contains("password") || target.contains("connection_string"))
                    throw Error("Credentials/connection strings must not be embedded in target JSON; use password_env "
                                "and libpq environment");
                result.push_back({.backend = parse_backend(name),
                                  .root = root,
                                  .artifacts = artifacts,
                                  .target = std::move(target),
                                  .verification = verification});
            }
            return result;
        }
        ExecutorFactory effective_factory(const ExecutorFactory &factory) {
            return factory ? factory : ExecutorFactory(detail::native_executor);
        }
    } // namespace
    void execute(const fs::path &manifest_path, const fs::path &target_path, bool confirm, unsigned concurrency,
                 const ExecutorFactory &factory, VerificationMode verification) {
        if (!confirm)
            throw Error("Execution requires --confirm-create for new isolated named targets; prepare never contacts "
                        "databases");
        if (concurrency == 0 || concurrency > 64)
            throw Error("Execution concurrency must be in [1,64]");
        const auto root = fs::absolute(manifest_path).parent_path();
        detail::DirectoryLock lock(root);
        const bool full_verification = verification == VerificationMode::full;
        const auto manifest = verify(manifest_path, full_verification), targets = read_json(target_path);
        const auto target_hash = sha256_text(targets.dump());
        const auto journal_path = root / "execution-state.json";
        Json journal = {{"schema_version", 1},
                        {"generation_id", manifest.at("generation_id")},
                        {"target_sha256", target_hash},
                        {"state", "preflight"},
                        {"backends", Json::object()}};
        if (fs::exists(journal_path)) {
            journal = read_json(journal_path);
            if (journal.at("generation_id") != manifest.at("generation_id") ||
                journal.at("target_sha256") != target_hash)
                throw Error("Execution journal is bound to a different generation/target; use a separate artifact "
                            "directory and isolated fresh targets");
            if (!full_verification)
                throw Error("An execution journal already exists. Import-only execution never reimports or resumes "
                            "existing targets; use verify-live to check a completed import");
            for (const auto &[name, state]: journal.at("backends").items())
                if (state.at("state") != "verified")
                    throw Error("Cannot safely resume " + name +
                                " after an interrupted/failed import: target may contain partial data. Provision fresh "
                                "isolated targets and a separate artifact directory; no automatic retries");
        }
        journal["verification"] = full_verification ? "full" : "skipped";
        const auto create = effective_factory(factory);
        auto configurations = contexts(root, manifest, targets, verification);
        std::vector<std::unique_ptr<IImportExecutor>> executors;
        // Entire preflight is read-only. A failure prevents every backend import.
        for (const auto &context: configurations) {
            auto executor = create(context);
            if (journal["backends"].contains(backend_name(context.backend)))
                (void) executor->verify();
            else
                executor->preflight();
            executors.push_back(std::move(executor));
        }
        write_json_atomic(journal_path, journal);
        const auto started = detail::Clock::now();
        std::atomic<std::size_t> next{0};
        std::mutex journal_mutex;
        std::exception_ptr error;
        const auto worker = [&] {
            for (;;) {
                const auto index = next.fetch_add(1);
                if (index >= executors.size())
                    return;
                const auto name = backend_name(configurations[index].backend);
                try {
                    {
                        std::scoped_lock guard(journal_mutex);
                        if (journal["backends"].contains(name))
                            continue;
                        // Durable intent precedes all remote mutation, including an empty-target import.
                        journal["backends"][name] = {{"state", "importing"}};
                        write_json_atomic(journal_path, journal);
                    }
                    auto imported = executors[index]->run();
                    const auto seconds = imported.value("native_import_seconds", 0.0);
                    if (seconds > 0) {
                        const auto &counts = configurations[index].artifacts.at("counts");
                        imported["records_per_second"] =
                                (counts.at("nodes").get<double>() + counts.at("proxies").get<double>() +
                                 counts.at("edges").get<double>()) /
                                seconds;
                        imported["bytes_per_second"] =
                                configurations[index].artifacts.at("bytes").get<double>() / seconds;
                    }
                    Json validated;
                    if (full_verification)
                        validated = executors[index]->verify();
                    std::scoped_lock guard(journal_mutex);
                    journal["backends"][name] = {{"state", full_verification ? "verified" : "imported"},
                                                 {"import", std::move(imported)}};
                    if (full_verification)
                        journal["backends"][name]["verification"] = std::move(validated);
                    write_json_atomic(journal_path, journal);
                } catch (...) {
                    std::scoped_lock guard(journal_mutex);
                    if (!error)
                        error = std::current_exception();
                    // A journal I/O failure must propagate through the main thread, never terminate a worker.
                    try {
                        journal["backends"][name] = {{"state", "failed"}};
                        write_json_atomic(journal_path, journal);
                    } catch (...) { /* error already retained; durable importing intent remains fail-closed */
                    }
                }
            }
        };
        {
            std::vector<std::jthread> workers;
            for (unsigned i = 0; i < std::min<std::size_t>(concurrency, executors.size()); ++i)
                workers.emplace_back(worker);
        }
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        journal["peak_rss_kb"] = usage.ru_maxrss;
        journal["seconds"] = detail::elapsed(started);
        journal["state"] = error ? "failed" : full_verification ? "verified" : "imported";
        write_json_atomic(journal_path, journal);
        if (error)
            std::rethrow_exception(error);
        if (full_verification) {
            Json report = {{"generation_id", manifest.at("generation_id")},
                           {"state", "verified"},
                           {"backends", Json::object()}};
            for (const auto &[name, backend]: journal.at("backends").items())
                report["backends"][name] = backend.at("verification");
            write_json_atomic(root / "live-verification.json", report);
        }
    }
    Json verify_live(const fs::path &manifest_path, const fs::path &target_path, const ExecutorFactory &factory) {
        const auto root = fs::absolute(manifest_path).parent_path();
        detail::DirectoryLock lock(root);
        const auto manifest = verify(manifest_path, true);
        const auto create = effective_factory(factory);
        Json report = {{"generation_id", manifest.at("generation_id")}, {"backends", Json::object()}};
        for (auto &context: contexts(root, manifest, read_json(target_path))) {
            const auto name = backend_name(context.backend);
            report["backends"][name] = create(std::move(context))->verify();
        }
        report["state"] = "verified";
        write_json_atomic(root / "live-verification.json", report);
        return report;
    }
} // namespace polygraph::ingestion
namespace polygraph::ingestion::detail {
    std::unique_ptr<IImportExecutor> native_executor(ImportContext context) {
        switch (context.backend) {
            case Backend::neo4j:
                return neo4j_executor(std::move(context));
            case Backend::arango:
                return arango_executor(std::move(context));
            case Backend::postgresql:
                return postgres_executor(std::move(context));
        }
        throw Error("Unsupported native executor");
    }
    std::string environment_secret(const Json &config, const std::string &key) {
        if (!config.contains(key))
            return "";
        const auto variable = config.at(key).get<std::string>();
        const auto *value = std::getenv(variable.c_str());
        if (!value)
            throw Error("Required environment variable is unset: " + variable);
        return value;
    }
    Json expected_counts(const ImportContext &context) { return context.artifacts.at("counts"); }
    bool expects_partition_metadata(const ImportContext &context) {
        return read_json(context.root / "manifest.json").at("configuration").contains("partition_metadata_version");
    }
    void verify_expected_rows(const ImportContext &c, const std::function<void(bool, const Json &)> &check) {
        // Bound requests and responses independently of graph size.
        const auto manifest = read_json(c.root / "manifest.json");
        Database db(artifact_path(c.root, manifest.at("index")), 4, true);
        const auto name = backend_name(c.backend);
        const bool partitioned = manifest.at("configuration").contains("partition_metadata_version");
        const std::string column = partitioned ? ",q.partition" : "";
        const std::string join = partitioned ? " JOIN partitions q ON q.key=n.key " : " ";
        auto nodes = db.statement(
                "SELECT n.id,n.type,0" + column + " FROM nodes n" + join + "WHERE n.owner=? UNION ALL SELECT n.id," +
                std::string(c.backend == Backend::postgresql ? "n.type" : "'Proxy'") + ",1" + column +
                " FROM proxies p JOIN nodes n ON n.id=p.node_id" + join + "WHERE p.backend=? ORDER BY 1");
        nodes.bind(1, name);
        nodes.bind(2, name);
        Json batch = Json::array();
        while (nodes.step()) {
            batch.push_back({{"id", nodes.integer(0)}, {"type", nodes.text(1)}, {"proxy", nodes.integer(2) != 0}});
            if (partitioned)
                batch.back()["partition_id"] = nodes.integer(3);
            if (batch.size() == 1000) {
                check(false, batch);
                batch = Json::array();
            }
        }
        if (!batch.empty())
            check(false, batch);
        auto edges = db.statement("SELECT e.id,e.from_id,e.to_id,e.type,e.owner" +
                                  std::string(partitioned ? ",f.partition,t.partition" : "") +
                                  " FROM replicas r JOIN edges e ON e.id=r.edge_id " +
                                  (partitioned ? "JOIN nodes fn ON fn.id=e.from_id JOIN nodes tn ON tn.id=e.to_id "
                                                 "JOIN partitions f ON f.key=fn.key JOIN partitions t ON t.key=tn.key "
                                               : "") +
                                  "WHERE r.backend=? ORDER BY e.id");
        edges.bind(1, name);
        batch = Json::array();
        while (edges.step()) {
            batch.push_back({{"id", edges.integer(0)},
                             {"from", edges.integer(1)},
                             {"to", edges.integer(2)},
                             {"type", edges.text(3)},
                             {"source", edges.text(4)}});
            if (partitioned) {
                batch.back()["from_partition_id"] = edges.integer(5);
                batch.back()["to_partition_id"] = edges.integer(6);
                batch.back()["is_partition_cut"] = edges.integer(5) != edges.integer(6);
            }
            if (batch.size() == 1000) {
                check(true, batch);
                batch = Json::array();
            }
        }
        if (!batch.empty())
            check(true, batch);
    }
} // namespace polygraph::ingestion::detail
