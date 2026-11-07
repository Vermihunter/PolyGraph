#include <map>
#include <set>
#include <thread>
#include "Native.hpp"
#include "ingestion/Process.hpp"
namespace polygraph::ingestion::detail {
    namespace {
        class Neo4j final : public IImportExecutor {
        public:
            explicit Neo4j(ImportContext context) :
                context_(std::move(context)), database_(context_.target.at("database").get<std::string>()) {}
            void preflight() override {
                if (!context_.target.value("offline", false))
                    throw Error("Neo4j full import requires offline=true and an isolated stopped DBMS/storage volume");
                if (!context_.target.contains("preflight_command"))
                    throw Error("Neo4j requires a preflight_command near its storage returning JSON {database, "
                                "offline:true, empty:true}");
                const auto result = run_process(context_.target.at("preflight_command").get<std::vector<std::string>>(),
                                                context_.root / "logs/neo4j-preflight");
                if (result.exit_code != 0)
                    throw Error("Neo4j offline/fresh-target preflight failed (exit " +
                                std::to_string(result.exit_code) + "); see logs/neo4j-preflight.stderr.log");
                Json check;
                try {
                    check = Json::parse(result.stdout_text);
                } catch (const Json::exception &) {
                    throw Error("Neo4j preflight must return a single JSON object on stdout");
                }
                if (check.at("database") != database_ || !check.value("offline", false) || !check.value("empty", false))
                    throw Error("Neo4j target is not provably offline and fresh");
                // Endpoint is required for post-import verification after the operator-supplied start runner.
                if (!context_.target.contains("endpoint"))
                    throw Error("Neo4j requires endpoint for post-import verification; optionally provide "
                                "start_command to start the isolated DBMS after import");
                (void) environment_secret(context_.target, "password_env");
            }
            Json run() override {
                const auto run_start = Clock::now();
                preflight();
                auto argv = context_.target.value("runner", std::vector<std::string>{"neo4j-admin"});
                if (argv.empty())
                    throw Error("Neo4j runner is empty");
                argv.insert(argv.end(),
                            {"database", "import", "full", database_, "--overwrite-destination=false",
                             "--skip-bad-relationships=false", "--skip-duplicate-nodes=false", "--bad-tolerance=0",
                             "--id-type=string", "--input-encoding=UTF-8", "--ignore-empty-strings=false",
                             "--multiline-fields=true", "--array-delimiter=U+001F",
                             "--threads=" + std::to_string(context_.target.value("threads", 4))});
                const fs::path remote = context_.target.value("artifact_root", context_.root.string());
                for (const auto &artifact: context_.artifacts.at("artifacts")) {
                    if (artifact.at("kind") == "fixups")
                        continue;
                    const auto path = (remote / fs::path(artifact.at("path").get<std::string>())).generic_string();
                    if (path.find_first_of(",*?[](){}") != std::string::npos)
                        throw Error("Neo4j import artifact path contains importer pattern characters; use a simple "
                                    "artifact_root path");
                    argv.push_back(std::string(artifact.at("kind") == "edges" ? "--relationships=" : "--nodes=") +
                                   path);
                }
                const auto result = run_process(argv, context_.root / "logs/neo4j-import");
                if (result.exit_code != 0)
                    throw Error("neo4j-admin full import failed (exit " + std::to_string(result.exit_code) +
                                "); inspect logs/neo4j-import.*.log. Partial offline stores are never retried "
                                "automatically");
                const auto startup_start = Clock::now();
                if (context_.target.contains("start_command")) {
                    const auto start = run_process(context_.target.at("start_command").get<std::vector<std::string>>(),
                                                   context_.root / "logs/neo4j-start");
                    if (start.exit_code != 0)
                        throw Error("Neo4j start_command failed after offline import; no automatic retry");
                }
                const auto attempts = context_.target.value("startup_attempts", 60U);
                bool online = false;
                for (unsigned i = 0; i < attempts; ++i) {
                    try {
                        (void) cypher("RETURN 1");
                        online = true;
                        break;
                    } catch (const Error &) {
                        if (i + 1 < attempts)
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                }
                if (!online)
                    throw Error("Neo4j offline import completed, but online verification is unavailable; start the "
                                "isolated database and run verify-live. execute will not claim success");
                const auto startup_seconds = elapsed(startup_start);
                const auto indexes_start = Clock::now();
                (void) cypher("CREATE CONSTRAINT polygraph_vertex_local_id_unique IF NOT EXISTS FOR "
                              "(n:POLYGRAPHVertex) REQUIRE n.localId IS UNIQUE");
                (void) cypher(
                        "CREATE CONSTRAINT proxy_uuid_unique IF NOT EXISTS FOR (n:Proxy) REQUIRE n.uuid IS UNIQUE");
                std::set<std::string> edge_types;
                for (const auto &a: context_.artifacts.at("artifacts"))
                    if (a.at("kind") == "edges")
                        edge_types.insert(a.at("collection").get<std::string>());
                for (const auto &type: edge_types)
                    for (const auto *property: {"localId", "uuid"})
                        (void) cypher("CREATE INDEX polygraph_ingestion_" + type + "_" + property +
                                      " IF NOT EXISTS FOR ()-[r:" + type + "]-() ON (r." + property + ")");
                const auto indexes_seconds = elapsed(indexes_start);
                const auto repairs_start = Clock::now();
                // Batch representational repairs; node lookups use the identity constraint index.
                for (const auto &artifact: context_.artifacts.at("artifacts"))
                    if (artifact.at("kind") == "fixups") {
                        std::ifstream in(artifact_path(context_.root, artifact));
                        std::string line;
                        Json batch = Json::array();
                        bool edge = false;
                        std::size_t bytes = 0;
                        const auto flush = [&] {
                            if (batch.empty())
                                return;
                            const std::string match =
                                    edge ? "UNWIND $rows AS row MATCH ()-[n]->() WHERE CASE WHEN n.localId IS NOT NULL "
                                           "THEN n.localId+281474976710656 ELSE n.uuid END = row.id "
                                         : "UNWIND $rows AS row MATCH (n:POLYGRAPHVertex {localId:row.local_id}) ";
                            const auto fixed =
                                    cypher(match + "SET n += row.properties RETURN count(n)", {{"rows", batch}});
                            if (fixed.at(0).at("row").at(0).get<std::size_t>() != batch.size())
                                throw Error("Neo4j property repair count mismatch");
                            batch = Json::array();
                            bytes = 0;
                        };
                        while (std::getline(in, line)) {
                            auto patch = Json::parse(line);
                            const bool next_edge = patch.at("edge").get<bool>();
                            if (!batch.empty() &&
                                (next_edge != edge || batch.size() >= 1000 || bytes + line.size() > 8 * 1024 * 1024))
                                flush();
                            edge = next_edge;
                            patch["local_id"] = local_id(std::bit_cast<Id>(patch.at("id").get<std::int64_t>()));
                            bytes += line.size();
                            batch.push_back(std::move(patch));
                        }
                        if (!in.eof())
                            throw Error("Neo4j fixup artifact read failed");
                        flush();
                    }
                return {{"native_import_seconds", result.seconds},
                        {"native_execution_seconds", elapsed(run_start)},
                        {"startup_seconds", startup_seconds},
                        {"index_creation_seconds", indexes_seconds},
                        {"property_repair_seconds", elapsed(repairs_start)},
                        {"exit_code", result.exit_code},
                        {"mechanism", "neo4j-admin database import full"}};
            }
            Json verify() override {
                const bool partitioned = expects_partition_metadata(context_);
                const auto expected = expected_counts(context_);
                const auto nodes = cypher("MATCH (n:POLYGRAPHVertex) RETURN count(CASE WHEN n.is_proxy = false THEN 1 "
                                          "END),count(CASE WHEN n.is_proxy = true THEN 1 END)")
                                           .at(0)
                                           .at("row");
                const auto edges = cypher("MATCH ()-[r]->() RETURN count(r)").at(0).at("row").at(0);
                if (nodes.at(0) != expected.at("nodes") || nodes.at(1) != expected.at("proxies") ||
                    edges != expected.at("edges"))
                    throw Error("Neo4j node/edge/proxy counts differ from manifest");
                const auto all = cypher("MATCH (n) RETURN count(n)").at(0).at("row").at(0).get<std::uint64_t>();
                if (all != expected.at("nodes").get<std::uint64_t>() + expected.at("proxies").get<std::uint64_t>())
                    throw Error("Neo4j target has unrelated nodes");
                verify_expected_rows(context_, [&](bool edge, const Json &batch) {
                    std::map<std::pair<std::string, bool>, Json> groups;
                    for (auto row: batch) {
                        const auto id = std::bit_cast<Id>(row.at("id").get<std::int64_t>());
                        const bool native = owner(id) == Backend::neo4j;
                        row["lookup_id"] = native ? static_cast<std::int64_t>(local_id(id)) : signed_id(id);
                        const auto key = std::pair(
                                edge ? sanitize_name(row.at("type").get<std::string>()) : std::string{}, native);
                        if (!groups.contains(key))
                            groups[key] = Json::array();
                        groups[key].push_back(std::move(row));
                    }
                    for (const auto &[key, rows]: groups) {
                        const auto property = key.second ? "localId" : "uuid";
                        std::string query =
                                edge ? "UNWIND $rows AS x MATCH (a)-[r:" + key.first + " {" + property +
                                                ":x.lookup_id}]->(b) WHERE "
                                                "(CASE WHEN a.localId IS NOT NULL THEN a.localId+281474976710656 ELSE "
                                                "a.uuid END)=x.from AND "
                                                "(CASE WHEN b.localId IS NOT NULL THEN b.localId+281474976710656 ELSE "
                                                "b.uuid END)=x.to AND "
                                                "r.type=x.type AND r.source=x.source RETURN x.id,count(r)"
                                     : "UNWIND $rows AS x MATCH (n:" +
                                                std::string(key.second ? "POLYGRAPHVertex:__GraphNode"
                                                                       : "Proxy:POLYGRAPHVertex:__GraphNode") +
                                                " {" + property +
                                                ":x.lookup_id}) WHERE n.type=x.type AND n.is_proxy=x.proxy RETURN "
                                                "x.id,count(n)";
                        if (partitioned)
                            query.insert(query.find(" RETURN "), edge ? " AND r.from_partition_id=x.from_partition_id "
                                                                        "AND r.to_partition_id=x.to_partition_id "
                                                                        "AND r.is_partition_cut=x.is_partition_cut"
                                                                      : " AND n.partition_id=x.partition_id");
                        const auto result = cypher(query, {{"rows", rows}});
                        if (result.size() != rows.size())
                            throw Error("Neo4j canonical ID/type/endpoint/partition metadata mismatch");
                        for (const auto &row: result)
                            if (row.at("row").at(1) != 1)
                                throw Error("Neo4j duplicate persistent IDs");
                    }
                });
                Json verified = {{"counts", expected}, {"exact_ids", true}, {"endpoint_traversal", true}};
                if (partitioned)
                    verified["partition_metadata"] = true;
                return verified;
            }

        private:
            ImportContext context_;
            std::string database_;
            Json cypher(const std::string &query, const Json &parameters = Json::object()) {
                const auto result = http_json(
                        context_.target, "POST", "/db/" + database_ + "/tx/commit",
                        Json{{"statements", Json::array({{{"statement", query}, {"parameters", parameters}}})}}.dump());
                if (!result.at("errors").empty())
                    throw Error("Neo4j Cypher request reported errors: " + result.at("errors").dump());
                return result.at("results").at(0).at("data");
            }
        };
    } // namespace
    std::unique_ptr<IImportExecutor> neo4j_executor(ImportContext c) { return std::make_unique<Neo4j>(std::move(c)); }
} // namespace polygraph::ingestion::detail
