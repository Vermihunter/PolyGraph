#include "ingestion/GraphManifest.hpp"

#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace polygraph::ingestion;

namespace {
    class GraphManifestTest : public ::testing::Test {
    protected:
        fs::path root;
        fs::path path;
        Json document;
        void SetUp() override {
            auto pattern = (fs::temp_directory_path() / "polygraph-manifest-XXXXXX").string();
            const auto directory = ::mkdtemp(pattern.data());
            ASSERT_NE(directory, nullptr);
            root = directory;
            fs::create_directory(root / "config");
            path = root / "config/domain.json";
            const auto example = fs::path(POLYGRAPH_PROJECT_ROOT) / "docs/examples/ingestion/logical-graph";
            for (const auto *file: {"schema.json", "graph.jsonl", "placement.json"})
                fs::copy_file(example / file, root / file);
            write_json_atomic(root / "backends.json",
                              {{"backends", {{{"kind", "Neo4j"}}, {{"kind", "ArangoDB"}}, {{"kind", "PostgreSQL"}}}}});
            document = {{"format", "polygraph.domain"},
                        {"version", 1},
                        {"id", "manifest-test"},
                        {"paths",
                         {{"schema", "../schema.json"},
                          {"data", "../graph.jsonl"},
                          {"backendConfig", "../backends.json"},
                          {"placementConfig", "../placement.json"},
                          {"ingestionArtifacts", "../output/prepared"}}},
                        {"ingestion",
                         {{"adapter", "polygraph-jsonl"}, {"identity", {{"strategy", "preserve-canonical-v2"}}}}}};
            save();
        }
        void TearDown() override {
            fs::permissions(root, fs::perms::owner_all, fs::perm_options::add);
            fs::remove_all(root);
        }
        void save() const { write_json_atomic(path, document); }
        auto snapshot() const {
            std::map<std::string, fs::file_time_type> result{{root.string(), fs::last_write_time(root)}};
            for (const auto &entry: fs::recursive_directory_iterator(root))
                result.emplace(entry.path().string(), fs::last_write_time(entry.path()));
            return result;
        }
        void expect_error(const std::string &prefix, bool execution = false) {
            const auto before = snapshot();
            try {
                (void) validate_graph_manifest(load_graph_manifest(path), execution);
                FAIL() << "Expected " << prefix;
            } catch (const Error &e) {
                EXPECT_TRUE(std::string(e.what()).starts_with(prefix)) << e.what();
            }
            EXPECT_EQ(snapshot(), before);
        }
    };

    TEST_F(GraphManifestTest, ResolvesManifestRelativePathsAndValidatesWithoutCreatingOutput) {
        const auto before = snapshot();
        const auto graph = load_graph_manifest(path);
        EXPECT_EQ(graph.id, "manifest-test");
        EXPECT_EQ(graph.options.input, root / "graph.jsonl");
        EXPECT_EQ(graph.options.output_dir, root / "output/prepared");
        EXPECT_EQ(graph.options.adapter, "polygraph-jsonl");
        const auto report = validate_graph_manifest(graph);
        EXPECT_EQ(report["state"], "valid");
        EXPECT_EQ(report["source_files"], 1);
        EXPECT_EQ(report["identity_strategy"], "preserve-canonical-v2");
        EXPECT_EQ(snapshot(), before);
        EXPECT_FALSE(fs::exists(graph.options.output_dir));
    }

    TEST_F(GraphManifestTest, RejectsVersionsUnknownAdaptersOptionsAndIdentityRulesWithoutSideEffects) {
        const auto original = document;
        for (const auto &[field, value, prefix]: std::vector<std::tuple<std::string, Json, std::string>>{
                     {"/version", 2, "graph.version:"},
                     {"/ingestion/adapter", "missing", "graph.adapter:"},
                     {"/ingestion/identity/strategy", "invented", "graph.identity:"},
                     {"/ingestion/options/threads", -1, "graph.options:"},
                     {"/ingestion/options/threadz", 3, "graph.options:"}}) {
            SCOPED_TRACE(field);
            document = original;
            document[Json::json_pointer(field)] = value;
            save();
            expect_error(prefix);
        }
    }

    TEST_F(GraphManifestTest, LegacyRuntimeManifestNeedsExplicitAdditiveNativeConfiguration) {
        document.erase("ingestion");
        save();
        expect_error("graph.ingestion:");
    }

    TEST_F(GraphManifestTest, MissingSourceAndUnknownPlacementTypeFailClearly) {
        document["paths"]["sourceData"] = "../missing-source";
        save();
        expect_error("graph.input:");
        document["paths"].erase("sourceData");
        save();
        auto placement = read_json(root / "placement.json");
        placement["nodeSources"]["Typo"] = "Neo4J";
        write_json_atomic(root / "placement.json", placement);
        expect_error("graph.placement:");
    }

    TEST_F(GraphManifestTest, InvalidSchemaEndpointsAndJsonlRoutingAreRejected) {
        const auto original = read_json(root / "schema.json");
        auto schema = original;
        schema["relations"][0]["to"] = "Unknown";
        write_json_atomic(root / "schema.json", schema);
        expect_error("graph.schema:");
        write_json_atomic(root / "schema.json", original);
        std::ofstream(root / "graph.jsonl") << "{\"kind\":\"metadata\",\"idRouting\":[\"Neo4J\"]}\n";
        expect_error("graph.identity:");
    }

    TEST_F(GraphManifestTest, ArangoVertexAndEdgeCollectionNamesCannotCollide) {
        auto schema = read_json(root / "schema.json");
        schema["relations"][1]["name"] = "Gateway";
        write_json_atomic(root / "schema.json", schema);
        auto placement = read_json(root / "placement.json");
        placement["relationshipSources"].erase("REPORTS_TO");
        placement["relationshipSources"]["Gateway"] = "PostgreSQL";
        write_json_atomic(root / "placement.json", placement);
        expect_error("graph.schema:");
    }

    TEST_F(GraphManifestTest, UnwritableOutputAncestorFailsWithoutWriteProbe) {
        fs::create_directory(root / "blocked");
        fs::permissions(root / "blocked", fs::perms::owner_read | fs::perms::owner_exec);
        document["paths"]["ingestionArtifacts"] = "../blocked/prepared";
        save();
        expect_error("graph.output:");
        fs::permissions(root / "blocked", fs::perms::owner_all);
    }

    TEST_F(GraphManifestTest, OutputCannotContainGraphConfigurationOrSourceFiles) {
        document["paths"]["ingestionArtifacts"] = "..";
        save();
        expect_error("graph.output:");
    }

    TEST_F(GraphManifestTest, ManifestPreparationBindsSchemaAndDeclarationToGenerationIdentity) {
        const auto graph = load_graph_manifest(path);
        (void) validate_graph_manifest(graph);
        const auto manifest = prepare(graph.options);
        EXPECT_EQ(manifest["counts"], (Json{{"nodes", 4}, {"edges", 4}}));
        EXPECT_EQ(manifest["source"]["configuration_files"].size(), 3);
        EXPECT_NO_THROW((void) verify(graph.options.output_dir / "manifest.json", true));
        auto schema = read_json(graph.schema);
        schema["nodeTypes"][0]["properties"][0]["name"] = "source_id";
        write_json_atomic(graph.schema, schema);
        EXPECT_THROW((void) prepare(load_graph_manifest(path).options), Error);
    }

    TEST_F(GraphManifestTest, RawSourcePathDoesNotReplaceRuntimeDataAndNoSqliteScratchIsCreated) {
        const auto fixture = fs::path(POLYGRAPH_PROJECT_ROOT) / "tests/fixtures/ingestion";
        document["ingestion"]["adapter"] = "ldbc-snb";
        document["ingestion"]["identity"]["strategy"] = "ldbc-block31-v3";
        document["paths"]["sourceData"] = (fixture / "raw").string();
        document["paths"]["schema"] = "../ldbc-schema.json";
        document["paths"]["placementConfig"] = (fixture / "placement.json").string();
        write_json_atomic(root / "ldbc-schema.json",
                          {{"nodeTypes", {{{"name", "Person"}}, {{"name", "Post"}}, {{"name", "Place"}}}},
                           {"relations",
                            {{{"name", "PERSON_LIKES_POST"}, {"from", "Person"}, {"to", "Post"}},
                             {{"name", "POST_ISLOCATEDIN_PLACE"}, {"from", "Post"}, {"to", "Place"}},
                             {{"name", "POST_HASCREATOR_PERSON"}, {"from", "Post"}, {"to", "Person"}}}}});
        save();
        const auto before = snapshot();
        EXPECT_EQ(validate_graph_manifest(load_graph_manifest(path))["state"], "valid");
        EXPECT_EQ(document["paths"]["data"], "../graph.jsonl");
        EXPECT_EQ(snapshot(), before);
    }

    TEST_F(GraphManifestTest, ExecutionPrerequisitesNeverInvokeCommandsOrConnectToTargets) {
        const auto capabilities = native_executor_capabilities();
        if (!capabilities["Neo4J"].get<bool>() || !capabilities["PostgreSQL"].get<bool>())
            GTEST_SKIP() << "Optional native executor libraries are unavailable";
        const auto runner = root / "operator-check";
        std::ofstream(runner) << "#!/bin/sh\ntouch '" << (root / "MUST_NOT_EXIST").string() << "'\nexit 1\n";
        fs::permissions(runner, fs::perms::owner_all);
        Json targets = Json::object();
        for (const auto *backend: {"Neo4J", "ArangoDB", "PostgreSQL"}) {
            targets[backend] = {{"database", "manifest-test"},
                                {"endpoint", "http://127.0.0.1:1"},
                                {"password_env", "POLYGRAPH_MANIFEST_TEST_PASSWORD"}};
        }
        targets["Neo4J"].update(
                {{"offline", true}, {"runner", {runner.string()}}, {"preflight_command", {runner.string()}}});
        write_json_atomic(root / "targets.json", {{"targets", targets}});
        document["paths"]["targetConfig"] = "../targets.json";
        save();
        ::unsetenv("POLYGRAPH_MANIFEST_TEST_PASSWORD");
        expect_error("graph.execution:", true);
        ::setenv("POLYGRAPH_MANIFEST_TEST_PASSWORD", "test-only", 1);
        const auto before = snapshot();
        const auto report = validate_graph_manifest(load_graph_manifest(path), true);
        EXPECT_EQ(report["state"], "valid");
        EXPECT_EQ(snapshot(), before);
        EXPECT_FALSE(fs::exists(root / "MUST_NOT_EXIST"));
        for (const auto &[key, value]: std::vector<std::pair<std::string, Json>>{{"import_concurrency", 0},
                                                                                 {"import_batch_mb", 65},
                                                                                 {"import_buffer_mb", 0},
                                                                                 {"import_batch_mb", 32}}) {
            auto invalid = targets;
            invalid["ArangoDB"][key] = value;
            if (key == "import_batch_mb" && value == 32)
                invalid["ArangoDB"]["import_buffer_mb"] = 16;
            write_json_atomic(root / "targets.json", {{"targets", invalid}});
            expect_error("graph.execution:", true);
        }
        ::unsetenv("POLYGRAPH_MANIFEST_TEST_PASSWORD");
    }
} // namespace
