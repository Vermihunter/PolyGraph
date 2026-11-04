#include <map>
#include <set>
#include "Native.hpp"
namespace polygraph::ingestion::detail {
    namespace {
        class Arango final : public IImportExecutor {
        public:
            explicit Arango(ImportContext context) :
                context_(std::move(context)), base_("/_db/" + context_.target.at("database").get<std::string>()) {}
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
                std::uint64_t total = 0;
                // Documents before edges: Arango validates the referenced collection handles.
                for (const bool edges: {false, true})
                    for (const auto &artifact: context_.artifacts.at("artifacts")) {
                        if ((artifact.at("kind") == "edges") != edges)
                            continue;
                        const auto collection = artifact.at("collection").get<std::string>();
                        std::ifstream in(artifact_path(context_.root, artifact), std::ios::binary);
                        if (!in)
                            throw Error("Cannot open Arango artifact");
                        std::string body, line;
                        std::uint64_t count = 0;
                        const auto flush = [&] {
                            if (count == 0)
                                return;
                            const auto response =
                                    api("POST",
                                        "/_api/import?collection=" + collection +
                                                "&type=documents&onDuplicate=error&complete=true&details=true",
                                        body);
                            if (response.at("errors").get<std::uint64_t>() != 0 ||
                                response.at("created").get<std::uint64_t>() != count ||
                                response.value("ignored", 0) != 0 || response.value("updated", 0) != 0)
                                throw Error("Arango import returned errors or unexpected document counts");
                            total += count;
                            body.clear();
                            count = 0;
                        };
                        while (std::getline(in, line)) {
                            if (line.size() > 64 * 1024 * 1024)
                                throw Error("Arango artifact record exceeds 64 MiB execution limit");
                            if (body.size() + line.size() > 8 * 1024 * 1024)
                                flush();
                            body += line;
                            body += '\n';
                            ++count;
                        }
                        if (!in.eof())
                            throw Error("Arango artifact read failed");
                        flush();
                    }
                for (const auto &[name, type]: collections) {
                    (void) type;
                    (void) api("POST", "/_api/index?collection=" + name,
                               Json{{"type", "persistent"}, {"fields", {"uuid"}}, {"unique", true}}.dump());
                }
                return {{"native_import_seconds", elapsed(start)},
                        {"created", total},
                        {"mechanism", "HTTP /_api/import"}};
            }
            Json verify() override {
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
                        const auto query =
                                edge ? "FOR x IN @rows LET d=DOCUMENT(@collection,x.key) FILTER d != null AND d.uuid "
                                       "== x.id AND d.type == x.type AND d.source == x.source AND "
                                       "PARSE_IDENTIFIER(d._from).key == TO_STRING(x.fromKey) AND "
                                       "PARSE_IDENTIFIER(d._to).key == TO_STRING(x.toKey) AND DOCUMENT(d._from) != "
                                       "null AND DOCUMENT(d._to) != null RETURN x.id"
                                     : "FOR x IN @rows LET d=DOCUMENT(@collection,x.key) FILTER d != null AND d.uuid "
                                       "== x.id AND d.type == x.type AND (d.is_proxy == true) == x.proxy RETURN x.id";
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
                            throw Error("Arango canonical ID/type/endpoint traversal mismatch");
                    }
                });
                return {{"counts", expected_counts(context_)}, {"exact_ids", true}, {"endpoint_traversal", true}};
            }

        private:
            ImportContext context_;
            std::string base_;
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
