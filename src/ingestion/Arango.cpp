#include <map>
#include <set>
#include "Native.hpp"
#include "ingestion/BatchImport.hpp"
namespace polygraph::ingestion::detail {
    namespace {
        constexpr std::size_t mib = 1024 * 1024;
        unsigned tuning(const Json &target, const std::string &name, unsigned fallback, unsigned maximum) {
            if (!target.contains(name))
                return fallback;
            const auto &value = target.at(name);
            if (!value.is_number_integer() || value < 1 || value > maximum)
                throw Error("Arango " + name + " must be an integer in [1," + std::to_string(maximum) + "]");
            return value.get<unsigned>();
        }
        class Arango final : public IImportExecutor {
        public:
            explicit Arango(ImportContext context) :
                context_(std::move(context)), base_("/_db/" + context_.target.at("database").get<std::string>()),
                batch_bytes_(tuning(context_.target, "import_batch_mb", 1, 64) * mib),
                limits_{.workers = tuning(context_.target, "import_concurrency", 4, 32),
                        .max_queued_batches = 8,
                        .max_buffered_bytes = tuning(context_.target, "import_buffer_mb", 128, 1024) * mib} {
                if (limits_.max_buffered_bytes < batch_bytes_)
                    throw Error("Arango import_buffer_mb must be at least import_batch_mb");
            }
            void preflight() override { ensure_empty(); }
            Json run() override {
                const auto start = Clock::now();
                ensure_empty();
                // Atomic collection creation reserves this named target across artifact directories.
                (void) api("POST", "/_api/collection",
                           Json{{"name", "polygraph_bulk_import_lock"}, {"type", 2}}.dump());
                const auto generation = read_json(context_.root / "manifest.json").at("generation_id");
                (void) api("POST", "/_api/document/polygraph_bulk_import_lock",
                           Json{{"_key", "generation"}, {"generation_id", generation}}.dump());
                std::map<std::string, int> collections;
                for (const auto &artifact: context_.artifacts.at("artifacts")) {
                    const auto name = artifact.at("collection").get<std::string>();
                    const int type = artifact.at("kind") == "edges" ? 3 : 2;
                    if (collections.contains(name) && collections[name] != type)
                        throw Error("Arango node/edge collection name collision: " + name);
                    collections[name] = type;
                }
                for (const auto &[name, type]: collections) {
                    const auto existing = api("GET", "/_api/collection/" + name, {}, true);
                    if (existing.value("not_found", false))
                        (void) api("POST", "/_api/collection", Json{{"name", name}, {"type", type}}.dump());
                    else if (existing.at("type") != type)
                        throw Error("Arango collection has incompatible document/edge type: " + name);
                }
                const auto setup_seconds = elapsed(start);
                BatchImportStats totals;
                Json phases = Json::object();
                // A complete acknowledgement barrier separates documents/proxies from edges.
                // Indexes are built only after both phases have drained successfully.
                for (const bool edges: {false, true}) {
                    const auto phase_start = Clock::now();
                    BoundedBatchLoader loader(limits_, [this](const ImportBatch &batch) {
                        try {
                            const auto response =
                                    api("POST",
                                        "/_api/import?collection=" + batch.collection +
                                                "&type=documents&onDuplicate=error&complete=true&details=true",
                                        batch.body);
                            if (response.at("errors").get<std::uint64_t>() != 0 ||
                                response.at("created").get<std::uint64_t>() != batch.records ||
                                response.value("ignored", 0) != 0 || response.value("updated", 0) != 0)
                                throw Error("Import errors or unexpected acknowledgement counts");
                        } catch (const std::exception &error) {
                            throw Error("Arango batch import failed for " + batch.collection + ": " + error.what());
                        }
                    });
                    for (const auto &artifact: context_.artifacts.at("artifacts")) {
                        if ((artifact.at("kind") == "edges") != edges)
                            continue;
                        const auto collection = artifact.at("collection").get<std::string>();
                        std::ifstream in(artifact_path(context_.root, artifact), std::ios::binary);
                        if (!in)
                            throw Error("Cannot open Arango artifact");
                        const auto records =
                                stream_import_batches(in, collection, batch_bytes_, 64 * mib,
                                                      [&](ImportBatch batch) { loader.submit(std::move(batch)); });
                        if (records != artifact.at("records").get<std::uint64_t>())
                            throw Error("Arango artifact record count differs from manifest: " + collection);
                    }
                    const auto completed = loader.finish();
                    totals.completed_records += completed.completed_records;
                    totals.completed_batches += completed.completed_batches;
                    totals.peak_active_batches = std::max(totals.peak_active_batches, completed.peak_active_batches);
                    totals.peak_queued_batches = std::max(totals.peak_queued_batches, completed.peak_queued_batches);
                    totals.peak_buffered_bytes = std::max(totals.peak_buffered_bytes, completed.peak_buffered_bytes);
                    phases[edges ? "edge_import_seconds" : "document_import_seconds"] = elapsed(phase_start);
                }
                const auto index_start = Clock::now();
                for (const auto &[name, type]: collections) {
                    (void) type;
                    (void) api("POST", "/_api/index?collection=" + name,
                               Json{{"type", "persistent"}, {"fields", {"uuid"}}, {"unique", true}}.dump());
                }
                return {{"native_import_seconds", elapsed(start)},
                        {"created", totals.completed_records},
                        {"collection_setup_seconds", setup_seconds},
                        {"document_import_seconds", phases.at("document_import_seconds")},
                        {"edge_import_seconds", phases.at("edge_import_seconds")},
                        {"index_build_seconds", elapsed(index_start)},
                        {"batching",
                         {{"workers", limits_.workers},
                          {"batch_target_bytes", batch_bytes_},
                          {"max_buffered_bytes", limits_.max_buffered_bytes},
                          {"max_queued_batches", limits_.max_queued_batches},
                          {"completed_batches", totals.completed_batches},
                          {"peak_active_batches", totals.peak_active_batches},
                          {"peak_queued_batches", totals.peak_queued_batches},
                          {"peak_buffered_bytes", totals.peak_buffered_bytes}}},
                        {"mechanism", "HTTP /_api/import"}};
            }
            Json verify() override {
                const bool partitioned = expects_partition_metadata(context_);
                std::map<std::string, std::uint64_t> expected;
                for (const auto &artifact: context_.artifacts.at("artifacts"))
                    expected[artifact.at("collection").get<std::string>()] +=
                            artifact.at("records").get<std::uint64_t>();
                const auto actual = api("GET", "/_api/collection?excludeSystem=true");
                std::set<std::string> found;
                for (const auto &collection: actual.at("result")) {
                    const auto name = collection.at("name").get<std::string>();
                    if (name == "polygraph_bulk_import_lock") {
                        const auto marker = api("GET", "/_api/document/polygraph_bulk_import_lock/generation");
                        if (marker.at("generation_id") !=
                                    read_json(context_.root / "manifest.json").at("generation_id") ||
                            api("GET", "/_api/collection/polygraph_bulk_import_lock/count").at("count") != 1)
                            throw Error("Arango target reservation belongs to another generation");
                        continue;
                    }
                    const auto count =
                            api("GET", "/_api/collection/" + name + "/count").at("count").get<std::uint64_t>();
                    if (!expected.contains(name) || expected[name] != count)
                        throw Error("Arango collection count differs from manifest: " + name);
                    found.insert(name);
                }
                if (found.size() != expected.size())
                    throw Error("Arango is missing expected collections");
                verify_expected_rows(context_, [&](bool edge, const Json &batch) {
                    std::map<std::string, Json> groups;
                    for (const auto &r: batch) {
                        const auto collection = sanitize_name(r.at("type").get<std::string>());
                        if (!groups.contains(collection))
                            groups[collection] = Json::array();
                        auto row = r;
                        row["key"] = std::to_string(std::bit_cast<Id>(r.at("id").get<std::int64_t>()));
                        groups[collection].push_back(std::move(row));
                    }
                    for (const auto &[collection, rows]: groups) {
                        std::string query =
                                edge ? "FOR x IN @rows LET d=DOCUMENT(@collection,x.key) FILTER d != null AND d.uuid "
                                       "== x.id AND d.type == x.type AND d.source == x.source AND "
                                       "PARSE_IDENTIFIER(d._from).key == TO_STRING(x.fromKey) AND "
                                       "PARSE_IDENTIFIER(d._to).key == TO_STRING(x.toKey) AND DOCUMENT(d._from) != "
                                       "null AND DOCUMENT(d._to) != null RETURN x.id"
                                     : "FOR x IN @rows LET d=DOCUMENT(@collection,x.key) FILTER d != null AND d.uuid "
                                       "== x.id AND d.type == x.type AND (d.is_proxy == true) == x.proxy RETURN x.id";
                        if (partitioned)
                            query.insert(query.find(" RETURN "),
                                         edge ? " AND d.from_partition_id == x.from_partition_id AND d.to_partition_id "
                                                "== x.to_partition_id "
                                                "AND d.is_partition_cut == x.is_partition_cut"
                                              : " AND d.partition_id == x.partition_id");
                        auto values = rows;
                        if (edge)
                            for (auto &r: values) {
                                r["fromKey"] = std::to_string(std::bit_cast<Id>(r.at("from").get<std::int64_t>()));
                                r["toKey"] = std::to_string(std::bit_cast<Id>(r.at("to").get<std::int64_t>()));
                            }
                        const auto result = api("POST", "/_api/cursor",
                                                Json{{"query", query},
                                                     {"bindVars", {{"rows", values}, {"collection", collection}}},
                                                     {"batchSize", 1000}}
                                                        .dump());
                        if (result.at("result").size() != rows.size() || result.value("hasMore", false))
                            throw Error("Arango canonical ID/type/endpoint/partition metadata mismatch");
                    }
                });
                Json verified = {
                        {"counts", expected_counts(context_)}, {"exact_ids", true}, {"endpoint_traversal", true}};
                if (partitioned)
                    verified["partition_metadata"] = true;
                return verified;
            }

        private:
            ImportContext context_;
            std::string base_;
            const std::size_t batch_bytes_;
            const BatchImportLimits limits_;
            Json api(const std::string &method, const std::string &path, const std::string &body = {},
                     bool absent = false) {
                return http_json(context_.target, method, base_ + path, body, absent);
            }
            void ensure_empty() {
                const auto response = api("GET", "/_api/collection?excludeSystem=true");
                if (!response.at("result").empty())
                    throw Error("Arango import requires a fresh named database with no user collections (including "
                                "empty collections or previous reservations)");
                // Existing named database is provisioned by the operator; no automatic create/drop.
            }
        };
    } // namespace
    std::unique_ptr<IImportExecutor> arango_executor(ImportContext c) { return std::make_unique<Arango>(std::move(c)); }
} // namespace polygraph::ingestion::detail
