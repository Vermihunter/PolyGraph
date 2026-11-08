#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>
#include "../src/ingestion/Internal.hpp"
#include "ingestion/GraphManifest.hpp"
#include "ingestion/InputAdapterRegistry.hpp"

using namespace polygraph::ingestion;

namespace {
    SourceIdentity identity(std::string type, Json value, std::string space = "inventory") {
        return {std::move(space), std::move(type), std::move(value)};
    }

    TEST(SourceIdentityTest, KeysSeparateNamespaceTypeKindAndExactValueType) {
        const auto base = identity("Device", Json(std::uint64_t{7}));
        const auto key = source_identity_key(base);
        EXPECT_NE(key, source_identity_key(identity("Site", Json(std::uint64_t{7}))));
        EXPECT_NE(key, source_identity_key(identity("Device", Json(std::uint64_t{7}), "other")));
        EXPECT_NE(key, source_identity_key(identity("Device", Json(std::int64_t{7}))));
        EXPECT_NE(key, source_identity_key(identity("Device", "7")));
        EXPECT_NE(key, source_identity_key(base, true));
        EXPECT_NE(source_identity_key(identity("b:c", "d", "a")), source_identity_key(identity("c", "d", "a:b")));
        EXPECT_NE(source_identity_key(identity("Device", " 7")), source_identity_key(identity("Device", "7")));
        EXPECT_NO_THROW((void) source_identity_key(identity("Device", std::uint64_t{UINT64_MAX})));
        EXPECT_NO_THROW((void) source_identity_key(identity("Device", std::int64_t{INT64_MIN})));
        EXPECT_NO_THROW((void) source_identity_key(identity("Device", std::string("x\0y", 3))));
    }
    TEST(SourceIdentityTest, RejectsUnsupportedIdentityValuesAndEmptyScopes) {
        for (const auto &value: {Json(), Json(true), Json(7.5), Json::array({7}), Json::object()})
            EXPECT_THROW((void) source_identity_key(identity("Device", value)), Error);
        EXPECT_THROW((void) source_identity_key(identity("", "7")), Error);
        EXPECT_THROW((void) source_identity_key(identity("Device", "7", "")), Error);
    }

    class Rows final : public IGraphInputAdapter {
    public:
        std::vector<Record> vertices, edges;
        void nodes(const RecordConsumer &consume) override {
            for (const auto &r: vertices)
                consume(r);
        }
        void relationships(const RecordConsumer &consume) override {
            for (const auto &r: edges)
                consume(r);
        }
        Json metadata() const override { return Json::object(); }
    };
    class SourcePipelineTest : public ::testing::Test {
    protected:
        fs::path root;
        Rows rows;
        void SetUp() override {
            auto pattern = (fs::temp_directory_path() / "polygraph-identities-XXXXXX").string();
            const auto path = ::mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            root = path;
            std::ofstream(root / "source") << "immutable inventory\n";
            write_json_atomic(root / "placement.json",
                              {{"storageBackends", {"Neo4J", "ArangoDB", "PostgreSQL"}},
                               {"nodeSources", {{"Device", "Neo4J"}, {"Site", "ArangoDB"}, {"Sensor", "PostgreSQL"}}},
                               {"defaultRelationshipSource", "PostgreSQL"}});
            rows.vertices = {Record{.type = "Device", .identity = identity("Device", "7")},
                             Record{.type = "Site", .identity = identity("Site", "7")},
                             Record{.type = "Sensor", .identity = identity("Sensor", "7")}};
            rows.edges = {Record{.type = "LINK",
                                 .identity = identity("LINK", "edge"),
                                 .from_identity = identity("Device", "7"),
                                 .to_identity = identity("Site", "7")},
                          Record{.type = "LINK",
                                 .from_identity = identity("Site", "7"),
                                 .to_identity = identity("Sensor", "7")}};
        }
        void TearDown() override { fs::remove_all(root); }
        InputAdapterRegistry registry() {
            InputAdapterRegistry r;
            r.add({.name = "typed-test",
                   .factory = [this](const PrepareOptions &, const fs::path &) { return std::make_unique<Rows>(rows); },
                   .input_files = [](const PrepareOptions &o) { return std::vector<fs::path>{o.input}; },
                   .id_policy = InputIdPolicy::typed_external});
            return r;
        }
        PrepareOptions options(const InputAdapterRegistry &r) const {
            return {.adapter = "typed-test",
                    .input = root / "source",
                    .placement_config = root / "placement.json",
                    .output_dir = root / "out",
                    .input_adapters = &r};
        }
    };
    TEST_F(SourcePipelineTest, ResolvesMixedTypesAndForwardReferencesThroughAllWriters) {
        auto r = registry();
        const auto o = options(r);
        auto m = prepare(o);
        EXPECT_EQ(m["counts"], (Json{{"nodes", 3}, {"edges", 2}}));
        EXPECT_EQ(m["id_strategy"], "typed-external-v1");
        EXPECT_EQ(m["source"]["id_strategy"], "typed-external-v1");
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
        detail::Database db(o.output_dir / m["index"]["path"].get<std::string>(), 4, true);
        auto q = db.statement("SELECT count(DISTINCT id),count(DISTINCT key) FROM nodes");
        ASSERT_TRUE(q.step());
        EXPECT_EQ(q.integer(0), 3);
        EXPECT_EQ(q.integer(1), 3);
        for (const auto *backend: {"Neo4J", "ArangoDB", "PostgreSQL"})
            EXPECT_EQ(m["backends"][backend]["counts"]["nodes"], 1);
        EXPECT_EQ(prepare(o)["generation_id"], m["generation_id"]);
    }
} // namespace

namespace {
    TEST_F(SourcePipelineTest, EqualValuesAcrossNamespaceAndValueTypeStayDistinctOnOneOwner) {
        rows.vertices.clear();
        rows.edges.clear();
        for (auto value: {Json(std::int64_t{7}), Json(std::uint64_t{7}), Json("7"), Json(std::int64_t{INT64_MIN}),
                          Json(std::uint64_t{UINT64_MAX})})
            rows.vertices.push_back(Record{.type = "Device", .identity = identity("Device", value)});
        rows.vertices.push_back(Record{.type = "Device", .identity = identity("Device", "7", "other")});
        rows.edges.push_back(Record{.type = "LINK",
                                    .identity = identity("LINK", "7"),
                                    .from_identity = identity("Device", std::int64_t{7}),
                                    .to_identity = identity("Device", std::uint64_t{7})});
        auto r = registry();
        const auto o = options(r);
        auto m = prepare(o);
        EXPECT_EQ(m["counts"], (Json{{"nodes", 6}, {"edges", 1}}));
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
        detail::Database db(o.output_dir / m["index"]["path"].get<std::string>(), 4, true);
        auto q = db.statement("SELECT from_id,to_id FROM edges");
        ASSERT_TRUE(q.step());
        EXPECT_NE(q.integer(0), q.integer(1));
    }
    TEST_F(SourcePipelineTest, MissingEndpointReportsTypedContextWithoutPublishing) {
        rows.edges[0].to_identity = identity("Site", "absent", "other-source");
        auto r = registry();
        const auto o = options(r);
        try {
            (void) prepare(o);
            FAIL() << "Missing endpoint accepted";
        } catch (const Error &e) {
            const std::string message = e.what();
            EXPECT_NE(message.find("Unresolved relationship endpoint"), std::string::npos);
            EXPECT_NE(message.find("other-source"), std::string::npos);
            EXPECT_NE(message.find("absent"), std::string::npos);
            EXPECT_NE(message.find("LINK"), std::string::npos);
        }
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
    }
    TEST_F(SourcePipelineTest, DuplicateVerticesFailEvenForIdenticalRecords) {
        rows.vertices.push_back(rows.vertices.front());
        auto r = registry();
        const auto o = options(r);
        EXPECT_THROW((void) prepare(o), Error);
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
    }
    TEST_F(SourcePipelineTest, DuplicateExplicitEdgesFailButIdlessParallelOccurrencesSurvive) {
        rows.edges.push_back(rows.edges.front());
        auto r = registry();
        auto o = options(r);
        EXPECT_THROW((void) prepare(o), Error);
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
        rows.edges[0].identity.reset();
        rows.edges[2].identity.reset();
        o.output_dir = root / "parallel";
        const auto m = prepare(o);
        EXPECT_EQ(m["counts"]["edges"], 3);
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    }
    TEST_F(SourcePipelineTest, RejectsConflictingLogicalTypeOpaqueKeysAndBackendSource) {
        auto r = registry();
        auto o = options(r);
        rows.vertices[0].identity->type = "Site";
        EXPECT_THROW((void) prepare(o), Error);
        rows.vertices[0].identity->type = "Device";
        rows.vertices[0].key = "opaque";
        o.output_dir = root / "opaque";
        EXPECT_THROW((void) prepare(o), Error);
        rows.vertices[0].key.clear();
        rows.vertices[0].source = Backend::arango;
        o.output_dir = root / "source-conflict";
        EXPECT_THROW((void) prepare(o), Error);
    }
    TEST_F(SourcePipelineTest, PreservesExplicitCanonicalIdsAndRejectsCrossKindGlobalCollisions) {
        rows.vertices[0].id = make_id(Backend::neo4j, 1000);
        auto r = registry();
        auto o = options(r);
        auto m = prepare(o);
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
        detail::Database db(o.output_dir / m["index"]["path"].get<std::string>(), 4, true);
        auto q = db.statement("SELECT id FROM nodes WHERE type='Device'");
        ASSERT_TRUE(q.step());
        EXPECT_EQ(q.integer(0), signed_id(*rows.vertices[0].id));
        rows.edges[0].id = rows.vertices[0].id;
        rows.edges[0].source = Backend::neo4j;
        write_json_atomic(root / "placement.json",
                          {{"storageBackends", {"Neo4J", "ArangoDB", "PostgreSQL"}},
                           {"nodeSources", {{"Device", "Neo4J"}, {"Site", "ArangoDB"}, {"Sensor", "PostgreSQL"}}}});
        o.output_dir = root / "collision";
        EXPECT_THROW((void) prepare(o), Error);
    }
    TEST_F(SourcePipelineTest, DeterministicFreshPreparationKeepsMappingsAndArtifacts) {
        auto r = registry();
        auto o = options(r);
        const auto first = prepare(o);
        o.output_dir = root / "second";
        const auto second = prepare(o);
        for (const auto *backend: {"Neo4J", "ArangoDB", "PostgreSQL"})
            EXPECT_EQ(first["backends"][backend]["artifacts"], second["backends"][backend]["artifacts"]);
        EXPECT_EQ(first["index"]["sha256"], second["index"]["sha256"]);
    }
    TEST_F(SourcePipelineTest, ExternalPartitionsUseSameTypedKeysAndCanChooseOwnerBeforeAllocation) {
        auto r = registry();
        auto o = options(r);
        o.partition_file = root / "parts.jsonl";
        o.partition_map = root / "map.json";
        write_json_atomic(o.partition_map, {{"0", "Neo4J"}, {"1", "ArangoDB"}, {"2", "PostgreSQL"}});
        std::ofstream out(o.partition_file);
        for (std::size_t i = 0; i < rows.vertices.size(); ++i)
            out << Json{{"key", source_identity_key(*rows.vertices[i].identity)}, {"partition", (i + 1) % 3}}.dump()
                << '\n';
        out.close();
        auto m = prepare(o);
        EXPECT_EQ(m["partitioning"]["partition_cut_relationships"], 2);
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    }
    TEST_F(SourcePipelineTest, KaminparReplaysStructuredIdentities) {
        if (!kaminpar_available())
            GTEST_SKIP() << "Requires optional KaMinPar build";
        auto r = registry();
        auto o = options(r);
        o.partitioner = "kaminpar";
        o.partition_options.count = 2;
        const auto m = prepare(o);
        EXPECT_EQ(m["counts"]["nodes"], 3);
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    }
    TEST_F(SourcePipelineTest, GraphManifestSelectsTypedStrategyWithoutRecordInspection) {
        write_json_atomic(root / "schema.json",
                          {{"nodeTypes", Json::array({{{"name", "Device"}}, {{"name", "Site"}}, {{"name", "Sensor"}}})},
                           {"relations", Json::array({{{"name", "LINK"}, {"from", "Device"}, {"to", "Site"}}})}});
        write_json_atomic(
                root / "backends.json",
                {{"backends", Json::array({{{"kind", "Neo4J"}}, {{"kind", "ArangoDB"}}, {{"kind", "PostgreSQL"}}})}});
        write_json_atomic(
                root / "domain.json",
                {{"format", "polygraph.domain"},
                 {"version", 1},
                 {"id", "typed-test"},
                 {"paths",
                  {{"schema", "schema.json"},
                   {"data", "source"},
                   {"backendConfig", "backends.json"},
                   {"placementConfig", "placement.json"},
                   {"ingestionArtifacts", "out"}}},
                 {"ingestion", {{"adapter", "typed-test"}, {"identity", {{"strategy", "typed-external-v1"}}}}}});
        auto r = registry();
        const auto graph = load_graph_manifest(root / "domain.json", &r);
        EXPECT_EQ(graph.identity_strategy, "typed-external-v1");
        EXPECT_EQ(validate_graph_manifest(graph)["state"], "valid");
        EXPECT_FALSE(fs::exists(root / "out"));
    }
} // namespace

#include <sys/resource.h>
#include <sys/wait.h>
namespace {
    class StreamingRows final : public IGraphInputAdapter {
    public:
        explicit StreamingRows(std::uint64_t count) : count_(count) {}
        void nodes(const RecordConsumer &consume) override {
            for (std::uint64_t i = 0; i < count_; ++i)
                consume(Record{.type = "Device",
                               .properties = {{"padding", std::string(512, 'x')}},
                               .identity = identity("Device", i)});
        }
        void relationships(const RecordConsumer &consume) override {
            for (std::uint64_t i = 1; i < count_; ++i)
                consume(Record{.type = "LINK",
                               .from_identity = identity("Device", i - 1),
                               .to_identity = identity("Device", i)});
        }
        Json metadata() const override { return Json::object(); }

    private:
        std::uint64_t count_;
    };
    TEST_F(SourcePipelineTest, LargeStreamUsesDiskResolverWithBoundedPeakResidentMemory) {
        // A 200k-node stream carries >100 MiB of properties alone. Keeping records
        // or the full identity dictionary in memory violates this 80 MiB ceiling.
        constexpr std::uint64_t count = 200000;
        InputAdapterRegistry r;
        r.add({.name = "typed-test",
               .factory = [count](const PrepareOptions &,
                                  const fs::path &) { return std::make_unique<StreamingRows>(count); },
               .input_files = [](const PrepareOptions &o) { return std::vector<fs::path>{o.input}; },
               .id_policy = InputIdPolicy::typed_external});
        auto o = options(r);
        o.memory_limit_mb = 16;
        o.max_record_mb = 1;
        const auto scratch = root / "stream-index";
        fs::create_directory(scratch);
        const auto child = ::fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            try {
                const auto m = detail::normalize(o, load_placement(o.placement_config), scratch);
                if (m["counts"]["nodes"] != count || m["counts"]["edges"] != count - 1)
                    ::_exit(2);
                detail::Database db(scratch / "graph.sqlite3", 4, true);
                auto q = db.statement(
                        "SELECT count(*) FROM edges e JOIN nodes f ON f.id=e.from_id JOIN nodes t ON t.id=e.to_id");
                if (!q.step() || q.integer(0) != count - 1)
                    ::_exit(3);
                ::_exit(0);
            } catch (const std::exception &e) {
                ::dprintf(STDERR_FILENO, "stream test: %s\n", e.what());
                ::_exit(4);
            }
        }
        int status = 0;
        struct rusage usage{};
        ASSERT_EQ(::wait4(child, &status, 0, &usage), child);
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);
        EXPECT_LT(usage.ru_maxrss, 80 * 1024) << "Peak RSS (KiB)=" << usage.ru_maxrss;
        EXPECT_GT(fs::file_size(scratch / "graph.sqlite3"), 100 * 1024 * 1024);
    }
} // namespace

namespace {
    TEST_F(SourcePipelineTest, GlobalCollisionDiagnosticIncludesLogicalTypeAndSourceIdentity) {
        rows.vertices.resize(2);
        rows.vertices[1].type = "Device";
        rows.vertices[1].identity = identity("Device", "another-device");
        for (auto &record: rows.vertices)
            record.id = make_id(Backend::neo4j, 1234);
        auto r = registry();
        const auto o = options(r);
        try {
            (void) prepare(o);
            FAIL() << "Duplicate GlobalId accepted";
        } catch (const Error &e) {
            const std::string message = e.what();
            EXPECT_NE(message.find("Device"), std::string::npos);
            EXPECT_NE(message.find("another-device"), std::string::npos);
            EXPECT_NE(message.find(std::to_string(*rows.vertices[1].id)), std::string::npos);
        }
    }
    TEST_F(SourcePipelineTest, LegacyIndexDoesNotAcquireTypedIdentityTables) {
        const auto fixture = fs::path(POLYGRAPH_PROJECT_ROOT) / "tests/fixtures/ingestion";
        for (const auto *name: {"polygraph-jsonl", "ldbc-snb"}) {
            PrepareOptions o{.adapter = name,
                             .input = fixture / (std::string_view(name) == "ldbc-snb" ? "raw" : "canonical.jsonl"),
                             .placement_config = fixture / "placement.json",
                             .output_dir = root / name};
            const auto m = prepare(o);
            detail::Database db(o.output_dir / m["index"]["path"].get<std::string>(), 4, true);
            auto q = db.statement("SELECT count(*) FROM sqlite_master WHERE name='source_edges'");
            ASSERT_TRUE(q.step());
            EXPECT_EQ(q.integer(0), 0);
        }
    }
} // namespace
