#include "ingestion/Import.hpp"
#include "ingestion/Process.hpp"

#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <latch>
#include <thread>
#include <unistd.h>

using namespace polygraph::ingestion;
namespace fs = std::filesystem;

class IngestionTest : public ::testing::Test {
protected:
    fs::path dir;
    void SetUp() override {
        static std::atomic<unsigned> sequence{0};
        dir = fs::temp_directory_path() /
              ("polygraph-ingestion-" + std::to_string(getpid()) + "-" + std::to_string(sequence++));
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }
    void write(const fs::path &path, const std::string &value) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << value;
    }
    PrepareOptions options() {
        write(dir / "placement.json",
              R"({"storageBackends":["Neo4J","ArangoDB","PostgreSQL"],"nodeSources":{"Person":"Neo4J","Post":"ArangoDB","Place":"PostgreSQL"},"relationshipSources":{"PERSON_LIKES_POST":"PostgreSQL","POST_ISLOCATEDIN_PLACE":"ArangoDB"}})");
        return {.adapter = "ldbc-snb",
                .input = dir / "raw",
                .placement_config = dir / "placement.json",
                .output_dir = dir / "out",
                .threads = 3,
                .memory_limit_mb = 32,
                .chunk_size_mb = 1,
                .max_record_mb = 4};
    }
    PrepareOptions fixture() {
        auto o = options();
        write(o.input / "dynamic/person_0_0.csv", "id|firstName|birthday\n1|\"Zoë|😀\"|2000-01-01\n");
        write(o.input / "dynamic/person_email_emailaddress_0_0.csv",
              "Person.id|email\n1|a@example.org\n1|b@example.org\n");
        write(o.input / "dynamic/post_0_0.csv", "id|content|length\n2|\"hello, \"\"world\"\"\nnext\"|12\n");
        write(o.input / "static/place_0_0.csv", "id|name\n3|Bratislava\n");
        write(o.input / "dynamic/person_likes_post_0_0.csv",
              "Person.id|Post.id|creationDate\n1|2|2020-01-01T00:00:00.000+0000\n");
        write(o.input / "dynamic/post_isLocatedIn_place_0_0.csv", "Post.id|Place.id\n2|3\n");
        return o;
    }
};

TEST_F(IngestionTest, ThreeBackendArtifactsAndRecovery) {
    const auto o = fixture();
    const auto m = prepare(o);
    EXPECT_EQ(m.at("counts").at("nodes"), 3);
    EXPECT_EQ(m.at("counts").at("edges"), 2);
    EXPECT_EQ(m.at("backends").at("Neo4J").at("counts").at("proxies"), 1);
    EXPECT_EQ(m.at("backends").at("ArangoDB").at("counts").at("edges"), 2);
    EXPECT_EQ(m.at("backends").at("PostgreSQL").at("counts").at("proxies"), 2);
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    EXPECT_EQ(prepare(o).at("generation_id"), m.at("generation_id"));
}

TEST_F(IngestionTest, CodecMatchesPythonAndSignedStorage) {
    EXPECT_EQ(make_id(Backend::neo4j, 1), 281474976710657ULL);
    EXPECT_EQ(make_id(Backend::postgresql, 3), 9223372036854775811ULL);
    EXPECT_EQ(signed_id(9223372036854775811ULL), -9223372036854775805LL);
    EXPECT_EQ(owner(9223372036854775811ULL), Backend::postgresql);
    EXPECT_THROW((void) owner(0), Error);
    EXPECT_THROW((void) owner(844424930131969ULL), Error);
    EXPECT_EQ(canonical_id(make_id(Backend::neo4j, 9) | (8ULL << 48)), make_id(Backend::neo4j, 9));
}

TEST_F(IngestionTest, ExternalPartitionsPublishVersionedMetadataAndChecksummedAssignments) {
    auto o = fixture();
    o.partition_file = dir / "assignments.jsonl";
    o.partition_map = dir / "mapping.json";
    write(o.partition_file, "{\"key\":\"Person:1\",\"partition\":40}\n"
                            "{\"key\":\"Post:2\",\"partition\":41}\n"
                            "{\"key\":\"Place:3\",\"partition\":42}\n");
    write(o.partition_map, R"({"40":"Neo4J","41":"ArangoDB","42":"PostgreSQL"})");
    const auto m = prepare(o);
    ASSERT_TRUE(m["configuration"].contains("partition_metadata_version"));
    EXPECT_EQ(m["configuration"]["partition_metadata_version"], 1);
    ASSERT_TRUE(m["partitioning"].contains("assignment_artifact"));
    const auto &assignment = m["partitioning"]["assignment_artifact"];
    EXPECT_EQ(assignment["records"], 3);
    EXPECT_EQ(sha256_file(o.output_dir / assignment["path"].get<std::string>()), assignment["sha256"]);
    EXPECT_EQ(m["partitioning"]["partition_cut_relationships"], 2);
    for (const auto &artifact: m["backends"]["Neo4J"]["artifacts"]) {
        std::ifstream stream(o.output_dir / artifact["path"].get<std::string>());
        std::string contents{std::istreambuf_iterator<char>(stream), {}};
        if (artifact["kind"] == "nodes" || artifact["kind"] == "proxies")
            EXPECT_NE(contents.find("partition_id:long"), std::string::npos);
        if (artifact["kind"] == "edges")
            EXPECT_NE(contents.find("is_partition_cut:boolean"), std::string::npos);
    }
    for (const auto &artifact: m["backends"]["ArangoDB"]["artifacts"]) {
        std::ifstream stream(o.output_dir / artifact["path"].get<std::string>());
        std::string line;
        while (std::getline(stream, line)) {
            const auto row = Json::parse(line);
            if (artifact["kind"] == "edges") {
                ASSERT_TRUE(row.contains("from_partition_id") && row.contains("to_partition_id") &&
                            row.contains("is_partition_cut"));
                EXPECT_TRUE(row["from_partition_id"].is_number_integer());
                EXPECT_TRUE(row["to_partition_id"].is_number_integer());
                EXPECT_EQ(row["is_partition_cut"], true);
            } else {
                ASSERT_TRUE(row.contains("partition_id"));
                EXPECT_TRUE(row["partition_id"].is_number_integer());
            }
        }
    }
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    write(o.output_dir / assignment["path"].get<std::string>(), "corrupt assignment artifact\n");
    EXPECT_THROW((void) verify(o.output_dir / "manifest.json", false), Error);
}

TEST_F(IngestionTest, ExternalPartitionsRejectFractionalNegativeAndUnknownAssignments) {
    for (const std::string value: {"40.5", "-4294967256", "\"40\""}) {
        auto o = fixture();
        o.output_dir = dir / ("out-" + sha256_text(value));
        o.partition_file = dir / "assignments.jsonl";
        o.partition_map = dir / "mapping.json";
        write(o.partition_file, "{\"key\":\"Person:1\",\"partition\":" + value +
                                        "}\n"
                                        "{\"key\":\"Post:2\",\"partition\":41}\n"
                                        "{\"key\":\"Place:3\",\"partition\":42}\n");
        write(o.partition_map, R"({"40":"Neo4J","41":"ArangoDB","42":"PostgreSQL"})");
        EXPECT_THROW((void) prepare(o), Error) << value;
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
    }
    auto o = fixture();
    o.partition_file = dir / "extra.jsonl";
    o.partition_map = dir / "mapping.json";
    write(o.partition_file, "{\"key\":\"Person:1\",\"partition\":40}\n"
                            "{\"key\":\"Post:2\",\"partition\":41}\n"
                            "{\"key\":\"Place:3\",\"partition\":42}\n"
                            "{\"key\":\"Person:999\",\"partition\":40}\n");
    EXPECT_THROW((void) prepare(o), Error);
}

TEST_F(IngestionTest, PartitionCheckpointRejectsMissingOrCorruptAssignmentsBeforePublication) {
    auto o = fixture();
    o.partition_file = dir / "assignments.jsonl";
    o.partition_map = dir / "mapping.json";
    write(o.partition_file, "{\"key\":\"Person:1\",\"partition\":0}\n"
                            "{\"key\":\"Post:2\",\"partition\":1}\n"
                            "{\"key\":\"Place:3\",\"partition\":2}\n");
    write(o.partition_map, R"({"0":"Neo4J","1":"ArangoDB","2":"PostgreSQL"})");
    for (const bool missing: {false, true}) {
        o.output_dir = dir / (missing ? "missing" : "corrupt");
        const auto manifest = prepare(o);
        fs::remove(o.output_dir / "manifest.json");
        const auto assignment =
                o.output_dir / manifest["partitioning"]["assignment_artifact"]["path"].get<std::string>();
        if (missing)
            fs::remove(assignment);
        else
            write(assignment, "corrupt\n");
        EXPECT_ANY_THROW((void) prepare(o));
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
    }
    o.output_dir = dir / "changed-before-publication";
    EXPECT_ANY_THROW((void) prepare(o, [&](std::string_view stage) {
        if (stage == "artifacts-ready") {
            const auto checkpoint = read_json(o.output_dir / "generation-state.json");
            write(o.output_dir /
                          checkpoint["normalization"]["partitioning"]["assignment_artifact"]["path"].get<std::string>(),
                  "corrupt\n");
        }
    }));
    EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
}

TEST_F(IngestionTest, PartitionMetadataCannotOverwriteInputProperties) {
    auto o = fixture();
    o.partition_file = dir / "assignments.jsonl";
    o.partition_map = dir / "mapping.json";
    write(o.partition_file, "{\"key\":\"Person:1\",\"partition\":40}\n"
                            "{\"key\":\"Post:2\",\"partition\":41}\n"
                            "{\"key\":\"Place:3\",\"partition\":42}\n");
    write(o.partition_map, R"({"40":"Neo4J","41":"ArangoDB","42":"PostgreSQL"})");
    write(o.input / "dynamic/person_0_0.csv", "id|partition_id\n1|999\n");
    EXPECT_THROW((void) prepare(o), Error);
    EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
}

TEST_F(IngestionTest, MissingEndpointAndCollisionDoNotPublish) {
    auto o = fixture();
    write(o.input / "dynamic/person_likes_post_0_0.csv", "from|to\n1|999\n");
    EXPECT_THROW((void) prepare(o), Error);
    EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
    write(o.input / "dynamic/person_likes_post_0_0.csv", "from|to\n1|2\n");
    write(o.input / "dynamic/person_0_1.csv", "id\n1\n");
    EXPECT_THROW((void) prepare(o), Error);
    EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
}

TEST_F(IngestionTest, CorruptArtifactAndStaleInputRejected) {
    auto o = fixture();
    auto m = prepare(o);
    const auto artifact = o.output_dir / m["backends"]["ArangoDB"]["artifacts"][0]["path"].get<std::string>();
    std::ofstream(artifact, std::ios::app) << "corrupt";
    EXPECT_THROW((void) verify(o.output_dir / "manifest.json", true), Error);
    EXPECT_THROW((void) prepare(o), Error);
}

TEST_F(IngestionTest, RejectsUnavailablePartitionerAndMalformedCsv) {
    auto o = fixture();
    o.partitioner = "kaminpar";
    EXPECT_THROW((void) prepare(o), Error);
    o.partitioner = "none";
    write(o.input / "dynamic/person_0_0.csv", "id|firstName\n1|\"unclosed\n");
    EXPECT_THROW((void) prepare(o), Error);
}

TEST_F(IngestionTest, SafeProcessUsesArgumentsAndReportsErrors) {
    const auto result = run_process({"/usr/bin/printf", "%s", "a;$(echo unsafe)"}, dir / "process");
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "a;$(echo unsafe)");
    EXPECT_NE(run_process({"/does/not/exist"}, dir / "missing").exit_code, 0);
    EXPECT_EQ(run_process({"/bin/sh", "-c", "echo failure >&2; exit 7"}, dir / "failed").exit_code, 7);
}

namespace {
    struct ExecutionState {
        std::atomic<int> imports{0};
        std::atomic<int> preflights{0};
        std::atomic<int> verifications{0};
        std::atomic<int> active{0};
        std::atomic<int> maximum{0};
        std::latch *started = nullptr;
        bool fail_preflight = false;
        bool fail_import = false;
        bool fail_verification = false;
    };
    class FakeExecutor final : public IImportExecutor {
    public:
        FakeExecutor(ImportContext context, ExecutionState &state) : context_(std::move(context)), state_(state) {}
        void preflight() override {
            ++state_.preflights;
            if (state_.fail_preflight)
                throw Error("target already contains data");
        }
        Json run() override {
            EXPECT_EQ(state_.preflights.load(), 3);
            const auto ticket = state_.imports.fetch_add(1);
            const auto active = ++state_.active;
            auto previous = state_.maximum.load();
            while (previous < active && !state_.maximum.compare_exchange_weak(previous, active)) {
            }
            if (state_.started && ticket < 2) {
                state_.started->count_down();
                state_.started->wait();
            }
            --state_.active;
            if (state_.fail_import)
                throw Error("import failed after partial write");
            return {{"native_import_seconds", 0.01}};
        }
        Json verify() override {
            ++state_.verifications;
            if (state_.fail_verification)
                throw Error("verification failed");
            return {{"counts", context_.artifacts.at("counts")}, {"exact_ids", true}};
        }

    private:
        ImportContext context_;
        ExecutionState &state_;
    };
} // namespace
TEST_F(IngestionTest, ExecutionRequiresConfirmationAndAllPreflightsBeforeImports) {
    auto o = fixture();
    (void) prepare(o);
    write(dir / "targets.json",
          R"({"targets":{"Neo4J":{"database":"bulk-test-neo"},"ArangoDB":{"database":"bulk_test_arango"},"PostgreSQL":{"database":"bulk_test_pg"}}})");
    ExecutionState state;
    ExecutorFactory factory = [&](ImportContext c) { return std::make_unique<FakeExecutor>(std::move(c), state); };
    EXPECT_THROW(execute(o.output_dir / "manifest.json", dir / "targets.json", false, 2, factory), Error);
    EXPECT_EQ(state.imports, 0);
    EXPECT_NO_THROW(execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory));
    EXPECT_EQ(state.imports, 3);
    EXPECT_NO_THROW(execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory));
    EXPECT_EQ(state.imports, 3);
}
TEST_F(IngestionTest, FailedExecutionIsNotRetriedAutomatically) {
    auto o = fixture();
    (void) prepare(o);
    write(dir / "targets.json",
          R"({"targets":{"Neo4J":{"database":"bulk-test-neo"},"ArangoDB":{"database":"bulk_test_arango"},"PostgreSQL":{"database":"bulk_test_pg"}}})");
    ExecutionState state;
    state.fail_import = true;
    ExecutorFactory factory = [&](ImportContext c) { return std::make_unique<FakeExecutor>(std::move(c), state); };
    EXPECT_THROW(execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory), Error);
    const auto imports = state.imports.load();
    EXPECT_THROW(execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory), Error);
    EXPECT_EQ(state.imports, imports);
}

TEST_F(IngestionTest, SkippedVerificationPublishesImportedStateAndNeverReimportsUncheckedTargets) {
    auto o = fixture();
    (void) prepare(o);
    write(dir / "targets.json",
          R"({"targets":{"Neo4J":{"database":"bulk-test-neo"},"ArangoDB":{"database":"bulk_test_arango"},"PostgreSQL":{"database":"bulk_test_pg"}}})");
    ExecutionState state;
    state.fail_verification = true;
    ExecutorFactory factory = [&](ImportContext c) {
        EXPECT_EQ(c.verification, VerificationMode::none);
        return std::make_unique<FakeExecutor>(std::move(c), state);
    };
    EXPECT_NO_THROW(
            execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory, VerificationMode::none));
    EXPECT_EQ(state.imports, 3);
    EXPECT_EQ(state.preflights, 3);
    EXPECT_EQ(state.verifications, 0);
    const auto journal = read_json(o.output_dir / "execution-state.json");
    EXPECT_EQ(journal.at("state"), "imported");
    EXPECT_EQ(journal.at("verification"), "skipped");
    for (const auto &backend: journal.at("backends")) {
        EXPECT_EQ(backend.at("state"), "imported");
        EXPECT_FALSE(backend.contains("verification"));
    }
    EXPECT_FALSE(fs::exists(o.output_dir / "live-verification.json"));
    EXPECT_THROW(
            execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory, VerificationMode::none),
            Error);
    EXPECT_EQ(state.imports, 3);
    EXPECT_EQ(read_json(o.output_dir / "execution-state.json"), journal);
}

TEST_F(IngestionTest, SkipVerificationStillRejectsPartialImportRetries) {
    auto o = fixture();
    (void) prepare(o);
    write(dir / "targets.json",
          R"({"targets":{"Neo4J":{"database":"bulk-test-neo"},"ArangoDB":{"database":"bulk_test_arango"},"PostgreSQL":{"database":"bulk_test_pg"}}})");
    ExecutionState state;
    state.fail_import = true;
    ExecutorFactory factory = [&](ImportContext c) { return std::make_unique<FakeExecutor>(std::move(c), state); };
    EXPECT_THROW(
            execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory, VerificationMode::none),
            Error);
    EXPECT_EQ(read_json(o.output_dir / "execution-state.json").at("state"), "failed");
    const auto attempts = state.imports.load();
    EXPECT_THROW(
            execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory, VerificationMode::none),
            Error);
    EXPECT_EQ(state.imports, attempts);
}

TEST_F(IngestionTest, SkipVerificationStillRejectsCorruptArtifactsBeforePreflight) {
    auto o = fixture();
    const auto manifest = prepare(o);
    write(dir / "targets.json",
          R"({"targets":{"Neo4J":{"database":"bulk-test-neo"},"ArangoDB":{"database":"bulk_test_arango"},"PostgreSQL":{"database":"bulk_test_pg"}}})");
    ExecutionState state;
    ExecutorFactory factory = [&](ImportContext c) { return std::make_unique<FakeExecutor>(std::move(c), state); };
    const auto artifact = o.output_dir / manifest["backends"]["ArangoDB"]["artifacts"][0]["path"].get<std::string>();
    std::ofstream(artifact, std::ios::app) << "corrupt";
    EXPECT_THROW(
            execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory, VerificationMode::none),
            Error);
    EXPECT_EQ(state.preflights, 0);
    EXPECT_EQ(state.imports, 0);
}

TEST_F(IngestionTest, VerifiedExecutionPublishesLiveReportAndStandaloneVerificationNeverImports) {
    auto o = fixture();
    (void) prepare(o);
    write(dir / "targets.json",
          R"({"targets":{"Neo4J":{"database":"bulk-test-neo"},"ArangoDB":{"database":"bulk_test_arango"},"PostgreSQL":{"database":"bulk_test_pg"}}})");
    ExecutionState state;
    ExecutorFactory factory = [&](ImportContext c) {
        EXPECT_EQ(c.verification, VerificationMode::full);
        return std::make_unique<FakeExecutor>(std::move(c), state);
    };
    execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory);
    EXPECT_EQ(state.verifications, 3);
    const auto report = read_json(o.output_dir / "live-verification.json");
    EXPECT_EQ(report.at("state"), "verified");
    EXPECT_EQ(report.at("backends").size(), 3);
    const auto journal = read_json(o.output_dir / "execution-state.json");
    (void) verify_live(o.output_dir / "manifest.json", dir / "targets.json", factory);
    EXPECT_EQ(state.verifications, 6);
    EXPECT_EQ(state.imports, 3);
    EXPECT_EQ(read_json(o.output_dir / "execution-state.json"), journal);
}

TEST_F(IngestionTest, CanonicalIdsResidencyArraysAndSignedEndpointsArePreserved) {
    auto o = options();
    o.adapter = "polygraph-jsonl";
    o.input = dir / "canonical.jsonl";
    const Id neo = make_id(Backend::neo4j, 11), arango = make_id(Backend::arango, 12),
             pg = make_id(Backend::postgresql, 13);
    Json metadata = {{"kind", "metadata"}, {"idRouting", {"Neo4J", "ArangoDB", "PostgreSQL"}}, {"idRoutingBits", 16}};
    Json a = {{"uuid", neo | (8ULL << 48)},
              {"type", "Person"},
              {"source", "Neo4J"},
              {"properties",
               {{"empty", Json::array()},
                {"special",
                 {"", "a\x1f"
                      "b"}},
                {"variant", 42},
                {"birthday", "2020-01-01"}}}};
    Json a2 = {{"uuid", make_id(Backend::neo4j, 14)},
               {"type", "Person"},
               {"source", "Neo4J"},
               {"properties", {{"variant", "a string"}}}};
    Json b = {{"uuid", arango},
              {"type", "Post"},
              {"source", "ArangoDB"},
              {"properties", {{"arr", {true, false}}, {"nested", {{"x", 1}}}}}};
    Json c = {{"uuid", std::to_string(signed_id(pg))},
              {"type", "Place"},
              {"source", "PostgreSQL"},
              {"properties", {{"quoted", "a,\"b\"\n😀"}}}};
    Json e = {{"uuid", make_id(Backend::postgresql, 99)},
              {"type", "PERSON_LIKES_POST"},
              {"source", "PostgreSQL"},
              {"from", neo},
              {"to", arango}};
    write(o.input, metadata.dump() + "\n" + a.dump() + "\n" + a2.dump() + "\n" + b.dump() + "\n" + c.dump() + "\n" +
                           e.dump() + "\n");
    const auto m = prepare(o);
    EXPECT_EQ(m["counts"]["nodes"], 4);
    bool fixups = false;
    for (const auto &artifact: m["backends"]["Neo4J"]["artifacts"])
        if (artifact["kind"] == "fixups")
            fixups = true;
    EXPECT_TRUE(fixups);
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    // A second residency decoration must collide with the same persistent identity.
    o.output_dir = dir / "duplicates";
    a["uuid"] = neo;
    std::ofstream(o.input, std::ios::app) << a.dump() << '\n';
    EXPECT_THROW((void) prepare(o), Error);
}
TEST_F(IngestionTest, InterruptedGenerationResumesOnlyCompletedBackends) {
    auto o = fixture();
    const auto m = prepare(o);
    fs::remove(o.output_dir / "manifest.json");
    auto stage = read_json(o.output_dir / "generation-state.json");
    stage["backends"].erase("ArangoDB");
    write_json_atomic(o.output_dir / "generation-state.json", stage);
    const auto native = o.output_dir / m["backends"]["Neo4J"]["artifacts"][0]["path"].get<std::string>();
    const auto timestamp = fs::last_write_time(native);
    write(o.output_dir / "interrupted.csv.tmp", "incomplete");
    EXPECT_NO_THROW((void) prepare(o));
    EXPECT_EQ(fs::last_write_time(native), timestamp);
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
}
TEST_F(IngestionTest, ExternalPartitionsMapOwnersBeforeIdsButEdgesKeepSchemaOwner) {
    auto o = fixture();
    o.partition_file = dir / "partitions.jsonl";
    o.partition_map = dir / "partition-map.json";
    write(o.partition_file, "{\"key\":\"Person:1\",\"partition\":40}\n{\"key\":\"Post:2\",\"partition\":41}\n{\"key\":"
                            "\"Place:3\",\"partition\":41}\n");
    write(o.partition_map, R"({"40":"ArangoDB","41":"Neo4J"})");
    const auto m = prepare(o);
    EXPECT_EQ(m["backends"]["ArangoDB"]["counts"]["nodes"], 1);
    EXPECT_EQ(m["backends"]["Neo4J"]["counts"]["nodes"], 2);
    EXPECT_EQ(m["backends"]["PostgreSQL"]["counts"]["edges"], 1);
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
}
TEST_F(IngestionTest, StrictVerificationRejectsForgedChecksumsAndMissingBackend) {
    auto o = fixture();
    auto m = prepare(o);
    auto &artifact = m["backends"]["ArangoDB"]["artifacts"][0];
    const auto path = o.output_dir / artifact["path"].get<std::string>();
    std::ofstream(path, std::ios::app) << "{}\n";
    artifact["sha256"] = sha256_file(path);
    artifact["bytes"] = fs::file_size(path);
    write_json_atomic(o.output_dir / "manifest.json", m);
    EXPECT_THROW((void) verify(o.output_dir / "manifest.json", true), Error);
    m["backends"].erase("PostgreSQL");
    write_json_atomic(o.output_dir / "manifest.json", m);
    EXPECT_THROW((void) verify(o.output_dir / "manifest.json", false), Error);
}
TEST_F(IngestionTest, HashMatchesKnownSha256Vector) {
    EXPECT_EQ(sha256_text("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST_F(IngestionTest, Neo4jDatabaseArgumentPrecedesVariadicFileOptions) {
    auto o = options();
    o.adapter = "polygraph-jsonl";
    o.input = dir / "one.jsonl";
    write(o.placement_config, R"({"storageBackends":["Neo4J"],"nodeSources":{"Person":"Neo4J"}})");
    write(o.input, "{\"kind\":\"metadata\",\"idRouting\":[\"Neo4J\"]}\n{\"uuid\":281474976710657,\"type\":\"Person\","
                   "\"source\":\"Neo4J\"}\n");
    (void) prepare(o);
    const Json target = {{"targets",
                          {{"Neo4J",
                            {{"database", "bulk-argument-test"},
                             {"startup_attempts", 0},
                             {"offline", true},
                             {"endpoint", "http://127.0.0.1:1"},
                             {"preflight_command",
                              {"/usr/bin/printf", R"({"database":"bulk-argument-test","offline":true,"empty":true})"}},
                             {"runner", {"/usr/bin/printf", "%s\n"}}}}}}};
    write_json_atomic(dir / "targets.json", target);
    EXPECT_THROW(execute(o.output_dir / "manifest.json", dir / "targets.json", true, 1), Error);
    std::ifstream in(o.output_dir / "logs/neo4j-import.stdout.log");
    std::string text{std::istreambuf_iterator<char>(in), {}};
    EXPECT_TRUE(text.starts_with("database\nimport\nfull\nbulk-argument-test\n"));
}

TEST_F(IngestionTest, ArangoImportTuningIsValidatedBeforeRemotePreflight) {
    auto o = fixture();
    (void) prepare(o);
    for (const auto &setting: std::vector<Json>{{{"import_concurrency", 0}},
                                                {{"import_concurrency", 33}},
                                                {{"import_concurrency", -1}},
                                                {{"import_concurrency", 1.5}},
                                                {{"import_concurrency", "4"}},
                                                {{"import_batch_mb", 0}},
                                                {{"import_buffer_mb", 0}},
                                                {{"import_batch_mb", 8}, {"import_buffer_mb", 1}}}) {
        auto target = setting;
        target["database"] = "bulk-config-arango";
        target["endpoint"] = "http://127.0.0.1:1";
        write_json_atomic(dir / "targets.json", {{"targets",
                                                  {{"ArangoDB", target},
                                                   {"Neo4J", {{"database", "bulk-config-neo"}}},
                                                   {"PostgreSQL", {{"database", "bulk_config_pg"}}}}}});
        try {
            execute(o.output_dir / "manifest.json", dir / "targets.json", true, 1, {}, VerificationMode::none);
            FAIL() << "Invalid import settings were accepted: " << setting;
        } catch (const Error &error) {
            EXPECT_NE(std::string(error.what()).find("import_"), std::string::npos) << error.what();
        }
        EXPECT_FALSE(fs::exists(o.output_dir / "execution-state.json"));
    }
}
TEST_F(IngestionTest, PostgreSqlProxiesRetainLogicalEndpointTypesForBfs) {
    auto o = fixture();
    const auto m = prepare(o);
    for (const auto &a: m["backends"]["PostgreSQL"]["artifacts"]) {
        std::ifstream file(o.output_dir / a["path"].get<std::string>());
        std::string text{std::istreambuf_iterator<char>(file), {}};
        EXPECT_EQ(text.find("\"Proxy\""), std::string::npos);
        if (a["kind"] == "nodes") {
            EXPECT_NE(text.find("\"Post\",\"PostgreSQL\",\"true\""), std::string::npos);
            EXPECT_NE(text.find("\"Person\",\"PostgreSQL\",\"true\""), std::string::npos);
        }
    }
}

TEST_F(IngestionTest, IndependentImportsRunConcurrentlyWithinTheConfiguredLimit) {
    auto o = fixture();
    (void) prepare(o);
    write(dir / "targets.json",
          R"({"targets":{"Neo4J":{"database":"bulk-test-neo"},"ArangoDB":{"database":"bulk_test_arango"},"PostgreSQL":{"database":"bulk_test_pg"}}})");
    ExecutionState state;
    std::latch started{2};
    state.started = &started;
    ExecutorFactory factory = [&](ImportContext c) { return std::make_unique<FakeExecutor>(std::move(c), state); };
    EXPECT_NO_THROW(execute(o.output_dir / "manifest.json", dir / "targets.json", true, 2, factory));
    EXPECT_EQ(state.maximum, 2);
    EXPECT_EQ(state.imports, 3);
}
TEST_F(IngestionTest, ChangedSourceAndEscapingArtifactPathsAreRejected) {
    auto o = fixture();
    auto m = prepare(o);
    write(o.input / "static/place_0_0.csv", "id|name\n3|changed\n");
    EXPECT_THROW((void) prepare(o), Error);
    m["backends"]["Neo4J"]["artifacts"][0]["path"] = "../escape.csv";
    write_json_atomic(o.output_dir / "manifest.json", m);
    EXPECT_THROW((void) verify(o.output_dir / "manifest.json"), Error);
}
TEST_F(IngestionTest, ChangedInputDuringGenerationCannotPoisonRecoveryCheckpoint) {
    auto o = fixture();
    const auto path = o.input / "static/place_0_0.csv";
    EXPECT_THROW((void) prepare(o,
                                [&](const std::string &stage) {
                                    if (stage == "source-hashed")
                                        write(path, "id|name\n3|different source\n");
                                }),
                 Error);
    write(path, "id|name\n3|Bratislava\n");
    EXPECT_THROW((void) prepare(o), Error);
    EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
}
TEST_F(IngestionTest, DuplicateNodePropertyHeadersAreRejected) {
    auto o = fixture();
    write(o.input / "dynamic/person_0_0.csv", "id|name|name\n1|first|second\n");
    EXPECT_THROW((void) prepare(o), Error);
}
TEST_F(IngestionTest, Neo4jSparseStringNullsGetRemovalRepairsAndSchemaUnionIsBounded) {
    auto o = options();
    o.adapter = "polygraph-jsonl";
    o.input = dir / "input.jsonl";
    write(o.placement_config, R"({"storageBackends":["Neo4J"],"nodeSources":{"Person":"Neo4J"}})");
    std::string records = "{\"kind\":\"metadata\",\"idRouting\":[\"Neo4J\"]}\n";
    for (unsigned i = 0; i < 4; ++i) {
        Json properties = Json::object();
        if (i == 0)
            properties["p"] = "x";
        if (i == 2)
            properties["p"] = nullptr;
        if (i == 3)
            properties["p"] = "";
        records += Json{{"uuid", make_id(Backend::neo4j, i)}, {"type", "Person"}, {"properties", properties}}.dump() +
                   "\n";
    }
    write(o.input, records);
    const auto m = prepare(o);
    std::size_t null_repairs = 0;
    for (const auto &a: m["backends"]["Neo4J"]["artifacts"])
        if (a["kind"] == "fixups") {
            std::ifstream in(o.output_dir / a["path"].get<std::string>());
            std::string line;
            while (std::getline(in, line))
                if (Json::parse(line)["properties"]["p"].is_null())
                    ++null_repairs;
        }
    EXPECT_EQ(null_repairs, 2);
    records = "{\"kind\":\"metadata\",\"idRouting\":[\"Neo4J\"]}\n";
    for (unsigned i = 0; i < 1025; ++i)
        records += Json{{"uuid", make_id(Backend::neo4j, i)},
                        {"type", "Person"},
                        {"properties", {{"unique_key_" + std::to_string(i), i}}}}
                           .dump() +
                   "\n";
    write(o.input, records);
    o.output_dir = dir / "schema-overflow";
    EXPECT_THROW((void) prepare(o), Error);
    EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
}
TEST_F(IngestionTest, SameDirectoryConcurrentPrepareFailsWithoutTouchingTheActiveGeneration) {
    auto o = fixture();
    std::latch locked{1}, resume{1};
    std::exception_ptr failure;
    std::jthread first([&] {
        try {
            (void) prepare(o, [&](const std::string &stage) {
                if (stage == "source-hashed") {
                    locked.count_down();
                    resume.wait();
                }
            });
        } catch (...) {
            failure = std::current_exception();
        }
    });
    locked.wait();
    EXPECT_THROW((void) prepare(o), Error);
    resume.count_down();
    first.join();
    EXPECT_EQ(failure, nullptr);
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
}
TEST_F(IngestionTest, UnicodeLabelSanitizationMatchesPythonCodePoints) {
    EXPECT_EQ(sanitize_name("Žilina😀"), "_ilina_");
    EXPECT_EQ(sanitize_name("9tag"), "X_9tag");
    EXPECT_EQ(sanitize_name(""), "X");
}
TEST_F(IngestionTest, MalformedCanonicalJsonlAndConflictingOwnershipFailBeforePublication) {
    auto o = options();
    o.adapter = "polygraph-jsonl";
    o.input = dir / "bad.jsonl";
    const std::string metadata = "{\"kind\":\"metadata\",\"idRouting\":[\"Neo4J\",\"ArangoDB\",\"PostgreSQL\"]}\n";
    const std::vector<std::string> invalid{"[1,2]\n",
                                           "{bad json}\n",
                                           "{\"kind\":\"unexpected\"}\n",
                                           "{\"uuid\":281474976710657,\"type\":\"Person\",\"source\":\"ArangoDB\"}\n",
                                           "{\"uuid\":844424930131969,\"type\":\"Person\"}\n",
                                           "{\"uuid\":281474976710657,\"type\":\"Person\",\"properties\":[]}\n"};
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        o.output_dir = dir / ("invalid-" + std::to_string(i));
        write(o.input, metadata + invalid[i]);
        EXPECT_THROW((void) prepare(o), Error);
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
    }
}
TEST_F(IngestionTest, GlobalIdPayloadBoundaryAndSignedDecimalRoundTrip) {
    const Id payload = (Id{1} << 48) - 1;
    const auto id = make_id(Backend::postgresql, payload);
    EXPECT_EQ(local_id(id), payload);
    EXPECT_EQ(parse_id(Json(std::to_string(id))), id);
    EXPECT_EQ(parse_id(Json(signed_id(id))), id);
    EXPECT_EQ(parse_id(Json(std::to_string(signed_id(id)))), id);
    EXPECT_THROW((void) make_id(Backend::neo4j, payload + 1), Error);
    EXPECT_THROW((void) parse_id(Json(1.5)), Error);
}
TEST_F(IngestionTest, ArangoNodeAndEdgeCollectionCollisionsFailPreparation) {
    auto o = options();
    o.adapter = "polygraph-jsonl";
    o.input = dir / "collection-collision.jsonl";
    write(o.placement_config,
          R"({"storageBackends":["ArangoDB"],"nodeSources":{"Person":"ArangoDB"},"relationshipSources":{"Person":"ArangoDB"}})");
    std::string records = "{\"kind\":\"metadata\",\"idRouting\":[\"ArangoDB\"]}\n";
    const auto id = make_id(Backend::arango, 1);
    records += Json{{"uuid", id}, {"type", "Person"}}.dump() + "\n";
    records += Json{{"uuid", id + 1}, {"type", "Person"}, {"from", id}, {"to", id}}.dump() + "\n";
    write(o.input, records);
    EXPECT_THROW((void) prepare(o), Error);
    EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
}

TEST_F(IngestionTest, LdbcDatasetRootIgnoresUpdateEventsAndGeneratorMetadata) {
    auto o = fixture();
    const auto snapshot = prepare(o);
    write(o.input / "params.ini", "ldbc.snb.datagen.serializer.numUpdatePartitions:1\n");
    write(o.input / "updateStream.properties", "update stream metadata, not a snapshot table\n");
    for (const auto *name: {"updateStream_0_0_forum.csv", "updateStream_0_0_person.csv", "UPDATESTREAM_1_0_person.csv",
                            "update_stream_2_0_person.csv"})
        write(o.input / name, "not snapshot CSV: \"unterminated update event\n");
    o.output_dir = dir / "with-update-streams";
    // Include a trailing separator, as users commonly do for dataset roots.
    o.input /= "";
    const auto mixed = prepare(o);
    EXPECT_EQ(mixed.at("generation_id"), snapshot.at("generation_id"));
    EXPECT_EQ(mixed.at("source"), snapshot.at("source"));
    EXPECT_EQ(mixed.at("counts"), snapshot.at("counts"));
    for (const auto &[backend, expected]: snapshot.at("backends").items()) {
        const auto &actual = mixed.at("backends").at(backend).at("artifacts");
        ASSERT_EQ(actual.size(), expected.at("artifacts").size());
        for (std::size_t i = 0; i < actual.size(); ++i)
            EXPECT_EQ(actual[i].at("sha256"), expected.at("artifacts")[i].at("sha256"));
    }
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    write(o.input / "updateStream_0_0_person.csv", "changed update events\n");
    EXPECT_EQ(prepare(o).at("generation_id"), mixed.at("generation_id"));
}

TEST_F(IngestionTest, LdbcUpdateStreamExceptionNeverHidesUnknownSnapshotTables) {
    auto o = fixture();
    unsigned attempt = 0;
    for (const auto *directory: {"static", "dynamic"}) {
        for (const auto *name: {"unknown_0_0.csv", "updateStream_0_0_person.csv",
                                "unexpected_updatestream_notes_0_0.csv", "update_stream_0_0_person.csv"}) {
            const auto path = o.input / directory / name;
            write(path, "id|name\n1|unknown snapshot\n");
            o.output_dir = dir / ("unknown-snapshot-" + std::to_string(attempt++));
            EXPECT_THROW((void) prepare(o), Error) << path;
            EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
            fs::remove(path);
        }
    }
}

TEST_F(IngestionTest, NativeFailuresIdentifyBackendAndExecutionPhase) {
    for (const auto &phase: {"preflight", "import", "verify"}) {
        auto o = fixture();
        o.output_dir = dir / phase;
        (void) prepare(o);
        write(dir / "targets.json",
              R"({"targets":{"Neo4J":{"database":"bulk-test-neo"},"ArangoDB":{"database":"bulk_test_arango"},"PostgreSQL":{"database":"bulk_test_pg"}}})");
        ExecutionState state;
        state.fail_preflight = std::string_view(phase) == "preflight";
        state.fail_import = std::string_view(phase) == "import";
        state.fail_verification = std::string_view(phase) == "verify";
        ExecutorFactory factory = [&](ImportContext c) { return std::make_unique<FakeExecutor>(std::move(c), state); };
        try {
            execute(o.output_dir / "manifest.json", dir / "targets.json", true, 1, factory);
            FAIL() << "Failure was swallowed in " << phase;
        } catch (const Error &e) {
            const std::string message = e.what();
            EXPECT_NE(message.find("backend=ArangoDB"), std::string::npos) << message;
            EXPECT_NE(message.find(std::string("phase=") + phase), std::string::npos) << message;
        }
        if (state.fail_preflight)
            EXPECT_EQ(state.imports, 0);
    }
}

TEST_F(IngestionTest, UnfamiliarMixedSchemaPreservesPropertiesParallelEdgesAndProxies) {
    const auto fixture_dir = fs::path(POLYGRAPH_PROJECT_ROOT) / "tests/fixtures/schema_agnostic";
    PrepareOptions o{.adapter = "polygraph-jsonl",
                     .input = fixture_dir / "graph.jsonl",
                     .placement_config = fixture_dir / "placement.json",
                     .output_dir = dir / "out"};
    const auto manifest = prepare(o);
    EXPECT_EQ(manifest.at("counts").at("nodes"), 4);
    EXPECT_EQ(manifest.at("counts").at("edges"), 6);
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    for (const auto *backend: {"Neo4J", "ArangoDB", "PostgreSQL"}) {
        EXPECT_GT(manifest["backends"][backend]["counts"]["nodes"].get<int>(), 0);
        EXPECT_GT(manifest["backends"][backend]["counts"]["edges"].get<int>(), 0);
        EXPECT_GT(manifest["backends"][backend]["counts"]["proxies"].get<int>(), 0);
    }
}

TEST_F(IngestionTest, UnfamiliarSchemaPartitionsUseCanonicalKeys) {
    const auto fixture_dir = fs::path(POLYGRAPH_PROJECT_ROOT) / "tests/fixtures/schema_agnostic";
    PrepareOptions o{.adapter = "polygraph-jsonl",
                     .input = fixture_dir / "graph.jsonl",
                     .placement_config = fixture_dir / "placement.json",
                     .output_dir = dir / "out"};
    o.partition_file = fixture_dir / "assignments.jsonl";
    o.partition_map = fixture_dir / "partition-map.json";
    const auto manifest = prepare(o);
    EXPECT_EQ(manifest["partitioning"]["assignment_artifact"]["records"], 4);
    EXPECT_EQ(manifest["partitioning"]["partition_cut_relationships"], 4);
    EXPECT_EQ(manifest["counts"]["edges"], 6);
    EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
}
