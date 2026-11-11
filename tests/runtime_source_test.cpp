#include <fstream>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/ingestion/Internal.hpp"
#include "algorithms/bfs/BfsExecutor.hpp"
#include "cli/BfsCli.hpp"
#include "cli/NativeNodeIdResolver.hpp"
#include "cli/SourceNodeIdResolver.hpp"
#include "ingestion/InputAdapterRegistry.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "support/ScriptedGraphStore.hpp"
#include "support/TestGraphStore.hpp"
#include "utilities/BackendIds.hpp"

using namespace polygraph;
namespace ing = polygraph::ingestion;
namespace {
    class ArgumentList {
    public:
        explicit ArgumentList(std::vector<std::string> values) : values_(std::move(values)) {
            for (auto &v: values_)
                argv_.push_back(v.data());
        }
        void parse(cli::CommandLine &c) { c.parse(static_cast<int>(argv_.size()), argv_.data()); }

    private:
        std::vector<std::string> values_;
        std::vector<char *> argv_;
    };
    class Rows final : public ing::IGraphInputAdapter {
    public:
        void nodes(const ing::RecordConsumer &consume) override {
            for (const auto *type: {"Device", "Site"})
                for (const auto &id: {json("sensor:7"), json(std::int64_t{7}), json(std::uint64_t{7})})
                    consume(ing::Record{.type = type, .identity = ing::SourceIdentity{"inventory", type, id}});
        }
        void relationships(const ing::RecordConsumer &consume) override {
            consume(ing::Record{.type = "INSTALLED_AT",
                                .from_identity = ing::SourceIdentity{"inventory", "Device", "sensor:7"},
                                .to_identity = ing::SourceIdentity{"inventory", "Site", "sensor:7"}});
        }
        ing::Json metadata() const override { return json::object(); }
    };
    class RuntimeSourceTest : public ::testing::Test {
    protected:
        std::filesystem::path root;
        json prepared;
        std::shared_ptr<DatabaseContext> context;
        void SetUp() override {
            auto pattern = (std::filesystem::temp_directory_path() / "polygraph-runtime-source-XXXXXX").string();
            auto path = ::mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            root = path;
            std::ofstream(root / "input") << "immutable source\n";
            ing::write_json_atomic(root / "placement.json", {{"storageBackends", {"Neo4J", "ArangoDB", "PostgreSQL"}},
                                                             {"defaultNodeSource", "Neo4J"},
                                                             {"defaultRelationshipSource", "Neo4J"}});
            ing::InputAdapterRegistry registry;
            registry.add({.name = "runtime-test",
                          .factory = [](const auto &, const auto &) { return std::make_unique<Rows>(); },
                          .input_files = [](const auto &o) { return std::vector<ing::fs::path>{o.input}; },
                          .id_policy = ing::InputIdPolicy::typed_external});
            prepared = ing::prepare({.adapter = "runtime-test",
                                     .input = root / "input",
                                     .placement_config = root / "placement.json",
                                     .output_dir = root / "artifacts",
                                     .input_adapters = &registry});
            context = DatabaseContext::create_empty();
            context->schema->add_node_type("Device");
            context->schema->add_node_type("Site");
            context->schema->add_edge_type("INSTALLED_AT", 1, 2);
            context->id_routing_backends = {NEO4J, ARANGO, POSTGRESQL};
            context->placement_catalog->set_id_routing_backends(context->id_routing_backends);
            context->register_store(
                    std::make_shared<test::TestGraphStore>(test::descriptor(NEO4J, 100, BackendKind::Neo4j)));
        }
        void TearDown() override { std::filesystem::remove_all(root); }
        NodeRef source(std::vector<std::string> flags) {
            cli::CommandLine c("runtime-test");
            cli::add_runtime_arguments(c);
            cli::add_bfs_arguments(c);
            flags.insert(flags.begin(), "runtime-test");
            ArgumentList args(std::move(flags));
            args.parse(c);
            return cli::parse_source(c, *context);
        }
        std::filesystem::path write_domain(const json &bfs) {
            ing::write_json_atomic(
                    root / "schema.json",
                    {{"nodeTypes", json::array({{{"name", "Device"}}, {{"name", "Site"}}})},
                     {"relations", json::array({{{"name", "INSTALLED_AT"}, {"from", "Device"}, {"to", "Site"}},
                                                {{"name", "OTHER_EDGE"}, {"from", "Device"}, {"to", "Site"}}})}});
            std::ofstream(root / "data.jsonl") << json{{"kind", "metadata"},
                                                       {"idRouting", {"Neo4J", "ArangoDB", "PostgreSQL"}},
                                                       {"idRoutingBits", 16},
                                                       {"idPayloadBits", 48}}
                                                          .dump()
                                               << '\n';
            std::filesystem::copy_file(std::filesystem::path(POLYGRAPH_PROJECT_ROOT) /
                                               "docs/examples/ingestion/logical-graph/backends.json",
                                       root / "backends.json", std::filesystem::copy_options::overwrite_existing);
            ing::write_json_atomic(root / "domain.json", {{"format", "polygraph.domain"},
                                                          {"version", 1},
                                                          {"id", "runtime-source"},
                                                          {"paths",
                                                           {{"schema", "schema.json"},
                                                            {"data", "data.jsonl"},
                                                            {"backendConfig", "backends.json"},
                                                            {"ingestionArtifacts", "artifacts"}}},
                                                          {"defaults", {{"bfs", bfs}}}});
            return root / "domain.json";
        }
        std::string selector(const char *type = "Device", const char *idtype = "string", json value = "sensor:7") {
            return json{{"namespace", "inventory"}, {"type", type}, {"idType", idtype}, {"id", value}}.dump();
        }
    };
    TEST_F(RuntimeSourceTest, ExactTypedRootsResolveAcrossLogicalTypesWithoutLdbcNames) {
        cli::SourceNodeIdResolver resolver(root / "artifacts/manifest.json");
        const auto a = resolver.resolve(ing::SourceIdentity{"inventory", "Device", "sensor:7"});
        const auto b = resolver.resolve(ing::SourceIdentity{"inventory", "Site", "sensor:7"});
        const auto signed_id = resolver.resolve(ing::SourceIdentity{"inventory", "Device", json(std::int64_t{7})});
        const auto unsigned_id = resolver.resolve(ing::SourceIdentity{"inventory", "Device", json(std::uint64_t{7})});
        EXPECT_NE(a.id, b.id);
        EXPECT_NE(signed_id.id, unsigned_id.id);
        EXPECT_EQ(a.owner, NEO4J);
        const auto parsed =
                source({"--start-source", selector(), "--source-index", (root / "artifacts/manifest.json").string()});
        EXPECT_EQ(parsed.global_id, a.id);
        EXPECT_EQ(parsed.type, context->schema->node_type_id("Device"));
    }
    TEST_F(RuntimeSourceTest, MissingOrInvalidSelectorsNeverFallBackToNumericOrPropertyIds) {
        for (const auto &value:
             {selector("Device", "string", "missing"), selector("Device", "float", 7.5), selector("Unknown")})
            EXPECT_THROW((void) source({"--start-source", value, "--source-index",
                                        (root / "artifacts/manifest.json").string()}),
                         std::exception);
        EXPECT_THROW((void) source({"--start-source", selector()}), std::exception);
        EXPECT_THROW((void) source({"--start-source", selector(), "--start-uuid", "7"}), std::exception);
    }
} // namespace

namespace {
    TEST_F(RuntimeSourceTest, DomainMetadataFeedsGenericFiltersAndRealBfsExecution) {
        const auto domain_path =
                write_domain({{"startSource", json::parse(selector())}, {"maxDepth", 2}, {"direction", "directed"}});
        cli::CommandLine command("runtime-test");
        cli::add_runtime_arguments(command);
        cli::add_bfs_arguments(command);
        ArgumentList arguments({"runtime-test", "--domain", domain_path.string(), "--allowed-edge-types",
                                "INSTALLED_AT", "--allowed-target-node-types", "Site", "--thread-count", "1"});
        arguments.parse(command);
        context = cli::GraphRuntimeBuilder::build(cli::parse_runtime_config(command));
        const auto start = cli::parse_source(command, *context);
        const auto device_type = context->schema->node_type_id("Device"),
                   site_type = context->schema->node_type_id("Site");
        cli::SourceNodeIdResolver resolver(root / "artifacts/manifest.json");
        const auto site = resolver.resolve({"inventory", "Site", "sensor:7"});
        auto graph = std::make_shared<test::ScriptedTopology>();
        graph->vertices = {{start.global_id, device_type}, {site.id, site_type}};
        graph->edges = {{graph->vertices[0], graph->vertices[1], context->schema->edge_type_id("INSTALLED_AT")}};
        auto store = std::make_shared<test::ScriptedGraphStore>(test::descriptor(NEO4J, 100, BackendKind::Neo4j), graph,
                                                                std::vector<GlobalId>{start.global_id, site.id});
        context->register_store(store);
        const auto spec = cli::parse_expand_spec(command, *context->schema);
        EXPECT_EQ(spec.allowed_edge_types, std::vector<TypeId>{context->schema->edge_type_id("INSTALLED_AT")});
        EXPECT_EQ(spec.allowed_target_node_types, std::vector<TypeId>{site_type});
        const auto options = cli::parse_bfs_options(command);
        const auto result = BfsExecutor(*context->placement_catalog).run(start, spec, options);
        ASSERT_EQ(result.distance.size(), 2);
        EXPECT_EQ(result.distance.at(start.global_id), 0);
        EXPECT_EQ(result.distance.at(site.id), 1);
        auto wrong_edge = spec;
        wrong_edge.allowed_edge_types = {context->schema->edge_type_id("OTHER_EDGE")};
        EXPECT_EQ(BfsExecutor(*context->placement_catalog).run(start, wrong_edge, options).distance.size(), 1);
        auto wrong_target = spec;
        wrong_target.allowed_target_node_types = {device_type};
        EXPECT_EQ(BfsExecutor(*context->placement_catalog).run(start, wrong_target, options).distance.size(), 1);
        auto both = spec;
        both.direction = Direction::Both;
        both.allowed_target_node_types.clear();
        EXPECT_EQ(BfsExecutor(*context->placement_catalog)
                          .run(NodeRef{site.id, site_type}, both, options)
                          .distance.at(start.global_id),
                  1);
    }
    TEST_F(RuntimeSourceTest, DomainDefaultsAndExplicitRootsHaveUnambiguousPrecedence) {
        const auto path = write_domain({{"startSource", json::parse(selector())}});
        cli::SourceNodeIdResolver resolver(root / "artifacts/manifest.json");
        EXPECT_EQ(source({"--domain", path.string()}).global_id,
                  resolver.resolve({"inventory", "Device", "sensor:7"}).id);
        const auto numeric = ing::make_id(ing::Backend::neo4j, 777);
        EXPECT_EQ(source({"--domain", path.string(), "--start-uuid", std::to_string(numeric)}).global_id, numeric);
        EXPECT_EQ(source({"--domain", path.string(), "--start-source", selector("Site")}).type,
                  context->schema->node_type_id("Site"));
        write_domain({{"startUuid", std::to_string(numeric)}});
        EXPECT_EQ(source({"--domain", path.string()}).global_id, numeric);
        EXPECT_EQ(source({"--domain", path.string(), "--start-source", selector()}).global_id,
                  resolver.resolve({"inventory", "Device", "sensor:7"}).id);
        write_domain({{"startUuid", std::to_string(numeric)}, {"startSource", json::parse(selector())}});
        EXPECT_THROW((void) source({"--domain", path.string()}), std::exception);
    }
    TEST_F(RuntimeSourceTest, ExplicitIdTypesPreserveSignedUnsignedAndRejectOverflowOrCoercion) {
        const auto index = (root / "artifacts/manifest.json").string();
        const auto a = source({"--start-source", selector("Device", "int64", 7), "--source-index", index});
        const auto b = source({"--start-source", selector("Device", "uint64", 7), "--source-index", index});
        EXPECT_NE(a.global_id, b.global_id);
        for (const auto &value:
             {selector("Device", "int64", json(std::uint64_t{UINT64_MAX})), selector("Device", "uint64", -1),
              selector("Device", "uint64", 7.5), selector("Device", "string", 7), selector("Device", "int64", "7")})
            EXPECT_THROW((void) source({"--start-source", value, "--source-index", index}), std::exception);
        for (const auto &value:
             {json{{"namespace", "n"}, {"type", "Device"}, {"idType", "int64"}, {"id", std::int64_t{INT64_MIN}}},
              json{{"namespace", "n"}, {"type", "Device"}, {"idType", "uint64"}, {"id", std::uint64_t{UINT64_MAX}}}})
            EXPECT_NO_THROW((void) domain::source_identity_from_json(value));
    }
    TEST_F(RuntimeSourceTest, LegacyGlobalAndNativeRemappedRootsRemainSupported) {
        const auto old = ing::make_id(ing::Backend::neo4j, 99), loaded = ing::make_id(ing::Backend::neo4j, 199);
        const auto map = root / "native-id-map.jsonl";
        std::ofstream out(map);
        out << json{{"record", "metadata"}, {"format", "polygraph.native-id-map"}, {"version", 1}}.dump() << '\n';
        out << json{{"record", "node"}, {"old", std::to_string(old)}, {"new", std::to_string(loaded)}}.dump() << '\n';
        out.close();
        auto resolver = std::make_shared<cli::NativeNodeIdResolver>(map);
        context->input_id_resolver = [resolver](GlobalId id) { return resolver->resolve(id); };
        const auto path = write_domain({{"startUuid", std::to_string(old)}});
        EXPECT_EQ(source({"--domain", path.string()}).global_id, loaded);
        EXPECT_EQ(source({"--start-uuid", std::to_string(old), "--source-index", "nonexistent"}).global_id, loaded);
        // Canonical roots never open ingestion artifacts, including legacy LDBC workspaces.
        std::filesystem::remove_all(root / "artifacts");
        EXPECT_EQ(source({"--domain", path.string()}).global_id, loaded);
    }
    TEST_F(RuntimeSourceTest, TypedLookupIsReadOnlyAndRejectsMissingNamespacesOrWrongRouting) {
        const auto artifacts = root / "artifacts";
        std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::string>> before;
        for (const auto &entry: std::filesystem::recursive_directory_iterator(artifacts))
            before[entry.path()] = {entry.last_write_time(),
                                    entry.is_regular_file() ? ing::sha256_file(entry.path()) : ""};
        const auto manifest = (artifacts / "manifest.json").string();
        EXPECT_NO_THROW((void) source({"--start-source", selector(), "--source-index", manifest}));
        cli::SourceNodeIdResolver resolver(manifest);
        EXPECT_THROW((void) resolver.resolve({"other", "Device", "sensor:7"}), std::exception);
        context->id_routing_backends = {ARANGO, POSTGRESQL};
        EXPECT_THROW((void) source({"--start-source", selector(), "--source-index", manifest}), std::exception);
        std::size_t count = 0;
        for (const auto &entry: std::filesystem::recursive_directory_iterator(artifacts)) {
            ++count;
            ASSERT_TRUE(before.contains(entry.path()));
            EXPECT_EQ(before[entry.path()].first, entry.last_write_time());
            EXPECT_EQ(before[entry.path()].second, entry.is_regular_file() ? ing::sha256_file(entry.path()) : "");
        }
        EXPECT_EQ(count, before.size());
    }
    TEST_F(RuntimeSourceTest, InvalidManifestStrategySizeAndEscapingIndexFailClosed) {
        auto bad = prepared;
        bad["id_strategy"] = "ldbc-block31-v3";
        ing::write_json_atomic(root / "artifacts/bad.json", bad);
        EXPECT_THROW(cli::SourceNodeIdResolver(root / "artifacts/bad.json"), std::exception);
        bad = prepared;
        bad["index"]["bytes"] = 0;
        ing::write_json_atomic(root / "artifacts/bad.json", bad);
        EXPECT_THROW(cli::SourceNodeIdResolver(root / "artifacts/bad.json"), std::exception);
        std::filesystem::copy_file(root / "artifacts" / prepared["index"]["path"].get<std::string>(),
                                   root / "outside.sqlite");
        bad = prepared;
        bad["index"]["path"] = "../outside.sqlite";
        ing::write_json_atomic(root / "artifacts/bad.json", bad);
        EXPECT_THROW(cli::SourceNodeIdResolver(root / "artifacts/bad.json"), std::exception);
    }
} // namespace

namespace {
    TEST_F(RuntimeSourceTest, StoredOwnerMismatchIsRejectedBeforeBfs) {
        const auto dbpath = root / "artifacts" / prepared["index"]["path"].get<std::string>();
        {
            ing::detail::Database db(dbpath, 1);
            db.exec("UPDATE nodes SET owner='ArangoDB' WHERE type='Device'");
        }
        EXPECT_THROW((void) source({"--start-source", selector(), "--source-index",
                                    (root / "artifacts/manifest.json").string()}),
                     std::exception);
    }
} // namespace

namespace {
    TEST_F(RuntimeSourceTest, BfsExecutableResolvesDomainSourceRootWithoutStartingServices) {
#ifndef POLYGRAPH_TEST_BFS_BINARY
        GTEST_SKIP() << "BFS executable not built";
#else
        const auto domain = write_domain({{"startSource", json::parse(selector())}}).string();
        const auto output = (root / "bfs.csv").string();
        const auto child = ::fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            ::execl(POLYGRAPH_TEST_BFS_BINARY, POLYGRAPH_TEST_BFS_BINARY, "--domain", domain.c_str(), "--max-depth",
                    "0", "--thread-count", "1", "--no-logging", "--no-events", "--output-file", output.c_str(),
                    static_cast<char *>(nullptr));
            ::_exit(127);
        }
        int status = 0;
        ASSERT_EQ(::waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
        std::ifstream in(output);
        std::string header, line;
        ASSERT_TRUE(std::getline(in, header));
        ASSERT_TRUE(std::getline(in, line));
        const auto expected = cli::SourceNodeIdResolver(root / "artifacts/manifest.json")
                                      .resolve({"inventory", "Device", "sensor:7"});
        EXPECT_EQ(header, "vertex_id;distance");
        EXPECT_EQ(line, std::to_string(expected.id) + ";0");
        EXPECT_FALSE(std::getline(in, line));
#endif
    }
    TEST(SourceIdentityCompatibilityTest, SharedEncoderKeepsExistingV1SourceKeyBytes) {
        const domain::SourceIdentity identity{"inventory", "Device", json(std::int64_t{7})};
        const std::string key = R"(["polygraph.source.v1","inventory","vertex","Device","int64",7])";
        EXPECT_EQ(domain::encode_source_identity(identity), key);
        EXPECT_EQ(ing::source_identity_key(identity), key);
    }
} // namespace

namespace {
    TEST_F(RuntimeSourceTest, TypedRootAlsoUsesExistingNativeIdRemapping) {
        const auto original = cli::SourceNodeIdResolver(root / "artifacts/manifest.json")
                                      .resolve({"inventory", "Device", "sensor:7"})
                                      .id;
        const auto loaded = ing::make_id(ing::Backend::neo4j, 999);
        const auto map = root / "native-map.jsonl";
        std::ofstream out(map);
        out << json{{"record", "metadata"}, {"format", "polygraph.native-id-map"}, {"version", 1}}.dump() << '\n';
        out << json{{"record", "node"}, {"old", std::to_string(original)}, {"new", std::to_string(loaded)}}.dump()
            << '\n';
        out.close();
        auto resolver = std::make_shared<cli::NativeNodeIdResolver>(map);
        context->input_id_resolver = [resolver](GlobalId id) { return resolver->resolve(id); };
        const auto node =
                source({"--start-source", selector(), "--source-index", (root / "artifacts/manifest.json").string()});
        EXPECT_EQ(node.global_id, loaded);
        EXPECT_EQ(node.type, context->schema->node_type_id("Device"));
    }
    TEST_F(RuntimeSourceTest, LdbcPreparedGraphKeepsLegacyRootAndCrossBackendBfsDistances) {
        const auto fixture = std::filesystem::path(POLYGRAPH_PROJECT_ROOT) / "tests/fixtures/ingestion";
        const auto manifest = ing::prepare({.adapter = "ldbc-snb",
                                            .input = fixture / "raw",
                                            .placement_config = fixture / "placement.json",
                                            .output_dir = root / "ldbc"});
        context = DatabaseContext::create_empty();
        context->schema->add_node_type("Person");
        context->schema->add_node_type("Post");
        context->schema->add_node_type("Place");
        context->schema->add_edge_type("PERSON_LIKES_POST", 1, 2);
        context->schema->add_edge_type("POST_HASCREATOR_PERSON", 2, 1);
        context->schema->add_edge_type("POST_ISLOCATEDIN_PLACE", 2, 3);
        context->id_routing_backends = {NEO4J, ARANGO, POSTGRESQL};
        context->placement_catalog->set_id_routing_backends(context->id_routing_backends);
        ing::detail::Database db(root / "ldbc" / manifest["index"]["path"].get<std::string>(), 1, true);
        auto graph = std::make_shared<test::ScriptedTopology>();
        std::map<GlobalId, NodeRef> by_id;
        std::map<std::string, GlobalId> keys;
        std::map<BackendId, std::vector<GlobalId>> owned;
        auto nodes = db.statement("SELECT id,type,key FROM nodes ORDER BY id");
        while (nodes.step()) {
            const auto id = std::bit_cast<GlobalId>(nodes.integer(0));
            NodeRef node{id, context->schema->node_type_id(nodes.text(1))};
            graph->vertices.push_back(node);
            by_id[id] = node;
            keys[nodes.text(2)] = id;
            owned[insertion::BackendEncodedId::decode(id, context->id_routing_backends)].push_back(id);
        }
        auto edges = db.statement("SELECT from_id,to_id,type FROM edges ORDER BY id");
        while (edges.step())
            graph->edges.push_back({by_id.at(std::bit_cast<GlobalId>(edges.integer(0))),
                                    by_id.at(std::bit_cast<GlobalId>(edges.integer(1))),
                                    context->schema->edge_type_id(edges.text(2))});
        for (const auto &[backend, ids]: owned)
            context->register_store(std::make_shared<test::ScriptedGraphStore>(test::descriptor(backend), graph, ids));
        const auto start = source({"--start-uuid", std::to_string(keys.at("Person:1"))});
        BfsOptions options;
        options.max_depth = 2;
        options.allow_native_pushdown = false;
        options.thread_count = 1;
        const auto result = BfsExecutor(*context->placement_catalog)
                                    .run(start, ExpandSpec{.direction = Direction::Undirected}, options);
        ASSERT_EQ(result.distance.size(), 3);
        EXPECT_EQ(result.distance.at(keys.at("Person:1")), 0);
        EXPECT_EQ(result.distance.at(keys.at("Post:2")), 1);
        EXPECT_EQ(result.distance.at(keys.at("Place:3")), 2);
    }
} // namespace
