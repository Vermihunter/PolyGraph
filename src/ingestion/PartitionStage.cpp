#include "Internal.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <numeric>

namespace polygraph::ingestion::detail {
    namespace {
        std::uint32_t partition_number(const Json &value) {
            if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<std::uint32_t>::max())
                throw Error("Partition ID must be a nonnegative uint32 integer");
            return value.get<std::uint32_t>();
        }

        void assignments(const fs::path &path, std::size_t max_record,
                         const std::function<void(const std::string &, std::uint32_t)> &consume) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw Error("Cannot read partition assignments: " + path.string());
            auto buffer = std::make_unique_for_overwrite<char[]>(max_record + 2);
            for (;;) {
                in.getline(buffer.get(), static_cast<std::streamsize>(max_record + 2));
                const auto extracted = static_cast<std::size_t>(in.gcount());
                if (in.bad())
                    throw Error("Partition assignment read failed");
                if (extracted == 0 && in.eof())
                    break;
                if (in.fail() && !in.eof())
                    throw Error("Partition assignment exceeds --max-record-mb");
                const auto bytes = extracted - (in.eof() ? 0 : 1);
                if (bytes > max_record)
                    throw Error("Partition assignment exceeds --max-record-mb");
                const std::string line(buffer.get(), bytes);
                if (line.find_first_not_of(" \t\r") != std::string::npos) {
                    try {
                        const auto record = Json::parse(line);
                        if (!record.is_object() || !record.contains("key") || !record["key"].is_string() ||
                            record["key"].get_ref<const std::string &>().empty() || !record.contains("partition"))
                            throw Error("Partition assignment requires a nonempty key and integer partition");
                        consume(record["key"].get<std::string>(), partition_number(record["partition"]));
                    } catch (const Json::exception &error) {
                        throw Error("Malformed partition assignment: " + std::string(error.what()));
                    }
                }
                if (in.eof())
                    break;
            }
        }

        void check_closure(Database &db) {
            auto missing = db.statement("SELECT n.key FROM nodes n LEFT JOIN partitions p ON p.key=n.key "
                                        "WHERE p.key IS NULL LIMIT 1");
            if (missing.step())
                throw Error("Missing partition assignment for " + missing.text(0));
            auto extra = db.statement("SELECT p.key FROM partitions p LEFT JOIN nodes n ON p.key=n.key "
                                      "WHERE n.key IS NULL LIMIT 1");
            if (extra.step())
                throw Error("Partition assignment references an unknown node: " + extra.text(0));
        }

        GraphTopology project(const PrepareOptions &o, Database &db, const fs::path &scratch, Json &metrics) {
            db.exec("CREATE TABLE partition_vertex_keys(key TEXT PRIMARY KEY) WITHOUT ROWID;"
                    "CREATE TABLE partition_vertices(vertex INTEGER PRIMARY KEY,key TEXT NOT NULL UNIQUE);"
                    "CREATE TABLE partition_edges(u INTEGER NOT NULL,v INTEGER NOT NULL,PRIMARY KEY(u,v)) WITHOUT "
                    "ROWID;");
            const auto auxiliary = scratch / "topology";
            fs::create_directories(auxiliary);
            auto adapter = make_adapter(o, auxiliary);
            std::uint64_t n = 0, original_edges = 0, loops = 0;
            {
                auto insert = db.statement("INSERT INTO partition_vertex_keys(key) VALUES(?)");
                db.exec("BEGIN");
                adapter->nodes([&](Record record) {
                    insert.bind(1, record.key);
                    insert.run();
                    if (++n >= std::numeric_limits<std::uint32_t>::max())
                        throw Error("Topology exceeds the supported 32-bit vertex index range");
                    if (n % 50000 == 0)
                        db.exec("COMMIT; BEGIN");
                });
                db.exec("COMMIT");
            }
            if (n == 0 || o.partition_options.count > n)
                throw Error("KaMinPar requires --partitions no larger than the nonempty snapshot vertex count");
            // Read an immutable table while constructing the ordered index.
            // Updating the table under its own SELECT cursor can repeat rows.
            // SQLite's temporary row numbers start at one; CSR coordinates do not.
            db.exec("BEGIN; INSERT INTO partition_vertices(key) SELECT key FROM partition_vertex_keys ORDER BY key;"
                    "DROP TABLE partition_vertex_keys; COMMIT;");
            {
                auto lookup = db.statement("SELECT vertex-1 FROM partition_vertices WHERE key=?");
                auto insert = db.statement("INSERT OR IGNORE INTO partition_edges VALUES(?,?)");
                const auto resolve = [&](const std::string &key) {
                    lookup.bind(1, key);
                    if (!lookup.step())
                        throw Error("Unresolved topology relationship endpoint: " + key);
                    const auto vertex = lookup.integer(0);
                    lookup.reset();
                    return vertex;
                };
                db.exec("BEGIN");
                adapter->relationships([&](Record record) {
                    const auto from = resolve(record.from), to = resolve(record.to);
                    if (from == to)
                        ++loops;
                    else {
                        insert.bind(1, std::min(from, to));
                        insert.bind(2, std::max(from, to));
                        insert.run();
                    }
                    if (++original_edges % 50000 == 0)
                        db.exec("COMMIT; BEGIN");
                });
                db.exec("COMMIT");
            }
            adapter.reset();
            fs::remove_all(auxiliary);
            auto count = db.statement("SELECT count(*) FROM partition_edges");
            if (!count.step())
                throw Error("Cannot count topology edges");
            const auto m = static_cast<std::uint64_t>(count.integer(0));
            // CSR + fill cursors + library input/copy + assignment/population
            // buffers. Library refinement workspace is additional, not hard-capped.
            const auto budget = static_cast<std::uint64_t>(o.memory_limit_mb) * 1024 * 1024;
            if (n > budget / 64 || m > (budget - n * 64) / 32)
                throw Error("Partition CSR/input buffers exceed --memory-limit-mb; increase the budget or use none");
            GraphTopology graph;
            graph.offsets.resize(n + 1);
            graph.neighbors.resize(2 * m);
            {
                auto edges = db.statement("SELECT u,v FROM partition_edges ORDER BY u,v");
                while (edges.step()) {
                    ++graph.offsets[static_cast<std::size_t>(edges.integer(0)) + 1];
                    ++graph.offsets[static_cast<std::size_t>(edges.integer(1)) + 1];
                }
                std::partial_sum(graph.offsets.begin(), graph.offsets.end(), graph.offsets.begin());
                auto cursor = graph.offsets;
                edges.reset();
                while (edges.step()) {
                    const auto u = static_cast<std::uint32_t>(edges.integer(0));
                    const auto v = static_cast<std::uint32_t>(edges.integer(1));
                    graph.neighbors[cursor[u]++] = v;
                    graph.neighbors[cursor[v]++] = u;
                }
            }
            metrics.update({{"vertices", n},
                            {"undirected_edges", m},
                            {"input_relationships", original_edges},
                            {"ignored_self_loops", loops},
                            {"duplicates_collapsed", original_edges - loops - m},
                            {"buffer_estimate_bytes", n * 64 + m * 32},
                            {"csr_bytes", graph.offsets.size() * sizeof(std::uint64_t) +
                                                  graph.neighbors.size() * sizeof(std::uint32_t)}});
            return graph;
        }
    } // namespace

    bool partition_metadata_enabled(const PrepareOptions &o) noexcept {
        return o.partitioner == "kaminpar" || !o.partition_file.empty();
    }

    Json load_partition_mapping(const PrepareOptions &o, const Placement &placement) {
        if (!o.partition_file.empty() && o.partition_map.empty())
            throw Error("--partition-file requires --partition-map");
        if (!o.partition_map.empty() && !partition_metadata_enabled(o))
            throw Error("--partition-map requires --partitioner kaminpar or --partition-file");
        if (o.partition_map.empty())
            return Json::object();
        auto mapping = read_json(o.partition_map);
        if (!mapping.is_object() || mapping.empty())
            throw Error("Partition map must be a nonempty object mapping partition numbers to backends");
        for (const auto &[key, value]: mapping.items()) {
            std::uint32_t number = 0;
            const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), number);
            if (error != std::errc{} || end != key.data() + key.size() || key != std::to_string(number))
                throw Error("Partition mapping keys must be canonical nonnegative uint32 decimals");
            if (!value.is_string())
                throw Error("Partition mapping values must be backend names");
            placement.validate(parse_backend(value.get<std::string>()));
            if (o.partitioner == "kaminpar" && number >= o.partition_options.count)
                throw Error("Partition map contains a block outside --partitions");
        }
        if (o.partitioner == "kaminpar")
            for (unsigned block = 0; block < o.partition_options.count; ++block)
                if (!mapping.contains(std::to_string(block)))
                    throw Error("Partition has no backend mapping: " + std::to_string(block));
        return mapping;
    }

    Json prepare_partition_assignments(const PrepareOptions &o, Database &db, const fs::path &scratch) {
        if (!partition_metadata_enabled(o))
            return {{"algorithm", "none"}, {"seed", o.seed}, {"seconds", 0}, {"edge_cut_measured", false}};
        const auto start = Clock::now();
        if (!o.partition_file.empty()) {
            auto insert = db.statement("INSERT INTO partitions VALUES(?,?)");
            db.exec("BEGIN");
            assignments(o.partition_file, static_cast<std::size_t>(o.max_record_mb) * 1024 * 1024,
                        [&](const std::string &key, const std::uint32_t block) {
                            insert.bind(1, key);
                            insert.bind(2, block);
                            insert.run();
                        });
            db.exec("COMMIT");
            return {{"algorithm", "external"},
                    {"seed", o.seed},
                    {"seconds", elapsed(start)},
                    {"edge_cut_measured", true}};
        }
        Json metrics = {{"algorithm", "kaminpar"},
                        {"seed", o.seed},
                        {"partitions", o.partition_options.count},
                        {"threads", o.partition_options.threads},
                        {"imbalance", o.partition_options.imbalance},
                        {"preset", o.partition_options.preset},
                        {"implementation", kaminpar_identity()},
                        {"deterministic", o.partition_options.threads == 1},
                        {"projection", "simple-undirected-unit-weight"},
                        {"vertex_order", "source-key-lexical"},
                        {"edge_cut_measured", true}};
        {
            const auto graph = project(o, db, scratch, metrics);
            metrics["projection_seconds"] = elapsed(start);
            const auto call_start = Clock::now();
            const auto result = partition_topology(graph, o.partition_options, o.seed);
            metrics["partition_call_seconds"] = elapsed(call_start);
            metrics["library_seconds"] = result.library_seconds;
            metrics["edge_cut"] = result.edge_cut;
            metrics["populations"] = result.populations;
            metrics["balanced"] = result.balanced;
            metrics["maximum_block_size"] = result.maximum_block_size;
            const auto assign_start = Clock::now();
            auto rows = db.statement("SELECT key,vertex-1 FROM partition_vertices ORDER BY key");
            auto insert = db.statement("INSERT INTO partitions VALUES(?,?)");
            db.exec("BEGIN");
            while (rows.step()) {
                insert.bind(1, rows.text(0));
                insert.bind(2, result.assignment.at(static_cast<std::size_t>(rows.integer(1))));
                insert.run();
            }
            db.exec("COMMIT");
            metrics["assignment_seconds"] = elapsed(assign_start);
        }
        db.exec("DROP TABLE partition_edges; DROP TABLE partition_vertices;");
        metrics["seconds"] = elapsed(start);
        return metrics;
    }

    void finish_partition_metadata(const PrepareOptions &o, Database &db, const fs::path &scratch, Json &metrics) {
        if (!partition_metadata_enabled(o))
            return;
        const auto start = Clock::now();
        check_closure(db);
        auto cuts = db.statement(
                "SELECT e.type,count(*),sum(f.partition!=t.partition) FROM edges e "
                "JOIN nodes fn ON fn.id=e.from_id JOIN nodes tn ON tn.id=e.to_id "
                "JOIN partitions f ON f.key=fn.key JOIN partitions t ON t.key=tn.key GROUP BY e.type ORDER BY e.type");
        Json relations = Json::array();
        std::uint64_t cut_relationships = 0;
        while (cuts.step()) {
            cut_relationships += static_cast<std::uint64_t>(cuts.integer(2));
            relations.push_back({{"type", cuts.text(0)},
                                 {"relationships", cuts.integer(1)},
                                 {"cut_relationships", cuts.integer(2)}});
        }
        metrics["relation_cuts"] = std::move(relations);
        metrics["partition_cut_relationships"] = cut_relationships;
        auto simple = db.statement("SELECT count(*) FROM (SELECT min(e.from_id,e.to_id),max(e.from_id,e.to_id) "
                                   "FROM edges e JOIN nodes fn ON fn.id=e.from_id JOIN nodes tn ON tn.id=e.to_id "
                                   "JOIN partitions f ON f.key=fn.key JOIN partitions t ON t.key=tn.key "
                                   "WHERE f.partition!=t.partition GROUP BY 1,2)");
        if (!simple.step())
            throw Error("Cannot compute independent partition cut");
        const auto simple_cut = static_cast<std::uint64_t>(simple.integer(0));
        if (metrics.contains("edge_cut") && metrics["edge_cut"] != simple_cut)
            throw Error("Partition cut changed between topology projection and endpoint resolution");
        metrics["edge_cut"] = simple_cut;
        const auto path = scratch / "partitions.jsonl";
        std::uint64_t count = 0;
        {
            AtomicOutput output(path);
            auto rows = db.statement("SELECT key,partition FROM partitions ORDER BY key");
            while (rows.step()) {
                output.write(Json{{"key", rows.text(0)}, {"partition", rows.integer(1)}}.dump() + "\n");
                ++count;
            }
            output.commit();
        }
        metrics["assignment_artifact"] = {{"path", fs::relative(path, o.output_dir).generic_string()},
                                          {"records", count},
                                          {"bytes", fs::file_size(path)},
                                          {"sha256", sha256_file(path)}};
        metrics["metadata_seconds"] = elapsed(start);
        metrics["seconds"] = metrics.at("seconds").get<double>() + elapsed(start);
    }

    void verify_partition_metadata(Database &db, const fs::path &root, const Json &manifest) {
        if (!manifest.at("configuration").contains("partition_metadata_version"))
            return; // Older manifests did not emit these additive fields.
        check_closure(db);
        const auto &artifact = manifest.at("partitioning").at("assignment_artifact");
        auto expected = db.statement("SELECT key,partition FROM partitions ORDER BY key");
        std::uint64_t count = 0;
        assignments(artifact_path(root, artifact),
                    manifest.at("limits").at("max_record_mb").get<std::size_t>() * 1024 * 1024,
                    [&](const std::string &key, const std::uint32_t block) {
                        if (!expected.step() || expected.text(0) != key || expected.integer(1) != block)
                            throw Error("Partition assignment artifact differs from normalized source-key mapping");
                        ++count;
                    });
        if (expected.step() || count != artifact.at("records") || count != manifest.at("counts").at("nodes"))
            throw Error("Partition assignment count differs from normalized nodes");
        auto nodes = db.statement("SELECT n.properties,p.partition FROM nodes n JOIN partitions p ON p.key=n.key");
        while (nodes.step()) {
            const auto props = Json::parse(nodes.text(0));
            if (!props.contains("partition_id") || !props["partition_id"].is_number_integer() ||
                props["partition_id"] != nodes.integer(1))
                throw Error("Node partition property differs from its assignment");
        }
        auto edges = db.statement("SELECT e.properties,f.partition,t.partition FROM edges e "
                                  "JOIN nodes fn ON fn.id=e.from_id JOIN nodes tn ON tn.id=e.to_id "
                                  "JOIN partitions f ON f.key=fn.key JOIN partitions t ON t.key=tn.key");
        while (edges.step()) {
            const auto props = Json::parse(edges.text(0));
            if (!props.contains("from_partition_id") || !props["from_partition_id"].is_number_integer() ||
                props["from_partition_id"] != edges.integer(1) || !props.contains("to_partition_id") ||
                !props["to_partition_id"].is_number_integer() || props["to_partition_id"] != edges.integer(2) ||
                !props.contains("is_partition_cut") || !props["is_partition_cut"].is_boolean() ||
                props["is_partition_cut"] != (edges.integer(1) != edges.integer(2)))
                throw Error("Relationship partition properties differ from endpoint assignments");
        }
    }
} // namespace polygraph::ingestion::detail
