#include <chrono>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cli/GraphRuntimeBuilder.hpp"
#include "cli/SourceNodeIdResolver.hpp"
#include "data_access/adapters/PostgreSQL/RelationalAttachment.hpp"
#include "ingestion/Import.hpp"
#include "ingestion/InputAdapterRegistry.hpp"
#include "ingestion/Process.hpp"
#include "utilities/BackendIds.hpp"

extern char **environ;

using namespace polygraph;
using namespace polygraph::postgres;
namespace {
    class RelationalAttachment : public ::testing::Test {
    protected:
        std::filesystem::path root;
        std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
        domain::LogicalGraphSchema schema;
        json document;
        std::string sql_schema;
        void sql(const std::string &s) {
            std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(connection.get(), s.c_str()), PQclear);
            ASSERT_TRUE(r);
            ASSERT_TRUE(PQresultStatus(r.get()) == PGRES_COMMAND_OK || PQresultStatus(r.get()) == PGRES_TUPLES_OK)
                    << PQresultErrorMessage(r.get());
        }
        void SetUp() override {
            auto pattern = (std::filesystem::temp_directory_path() / "polygraph-attach-XXXXXX").string();
            const auto path = mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            root = path;
            sql_schema = "identity_" + root.filename().string().substr(17);
            connection.reset(PQconnectdb(std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN")));
            ASSERT_EQ(PQstatus(connection.get()), CONNECTION_OK) << PQerrorMessage(connection.get());
            const auto table = quote_identifier(sql_schema);
            sql("CREATE SCHEMA " + table + "; CREATE TABLE " + table + ".person(id bigint PRIMARY KEY); CREATE TABLE " +
                table + ".item(id text PRIMARY KEY); CREATE TABLE " + table + ".post(id uuid PRIMARY KEY);");
            sql("INSERT INTO " + table + ".person VALUES(7),(-2),(9223372036854775807); INSERT INTO " + table +
                ".item VALUES('7'),('007'),('雪'); INSERT INTO " + table +
                ".post VALUES('A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11')");
            schema.add_node_type("Person");
            schema.add_node_type("Item");
            schema.add_node_type("Post");
            document = {{"format", "polygraph.postgresql-relational"},
                        {"version", 1},
                        {"relationships", json::array()},
                        {"nodes", json::array({{{"type", "Person"},
                                                {"schema", sql_schema},
                                                {"table", "person"},
                                                {"primaryKey", "id"},
                                                {"namespace", "app"},
                                                {"keyCodec", "int64"}},
                                               {{"type", "Item"},
                                                {"schema", sql_schema},
                                                {"table", "item"},
                                                {"primaryKey", "id"},
                                                {"namespace", "app"},
                                                {"keyCodec", "string"}},
                                               {{"type", "Post"},
                                                {"schema", sql_schema},
                                                {"table", "post"},
                                                {"primaryKey", "id"},
                                                {"namespace", "app"},
                                                {"keyCodec", "uuid"}}})}};
        }
        void TearDown() override {
            if (connection && PQstatus(connection.get()) == CONNECTION_OK)
                sql("RESET ROLE; ROLLBACK; DROP SCHEMA IF EXISTS " + quote_identifier(sql_schema) + " CASCADE");
            if (!root.empty())
                std::filesystem::remove_all(root);
        }
        RelationalMapping mapping() { return parse_relational_mapping(document, schema); }
        std::filesystem::path attach(std::string name, std::optional<std::filesystem::path> previous = {}) {
            return attach_relational_nodes(connection.get(), mapping(),
                                           {.output_dir = root / name, .previous_manifest = previous, .batch_size = 2});
        }
        ResolvedRelationalNode get(RelationalIdentityResolver &r, const char *type, json value) {
            const std::array<domain::SourceIdentity, 1> keys{{{"app", type, std::move(value)}}};
            return r.resolve(keys).at(0);
        }
    };
} // namespace
TEST_F(RelationalAttachment, ResolvesTypedKeysAndReverseLookupThroughRuntimeSourceRoot) {
    const auto manifest = attach("first");
    RelationalIdentityResolver resolver(manifest, mapping());
    const auto person = get(resolver, "Person", 7), item = get(resolver, "Item", "7");
    EXPECT_NE(person.node.global_id, item.node.global_id);
    EXPECT_EQ(person.owner, POSTGRESQL);
    const std::array<NodeRef, 1> refs{person.node};
    const auto reverse = resolver.reverse(refs);
    ASSERT_EQ(reverse.size(), 1);
    EXPECT_EQ(reverse[0].source.value, 7);
    EXPECT_TRUE(reverse[0].source.value.is_number_integer());
    EXPECT_FALSE(reverse[0].source.value.is_number_unsigned());
    const cli::SourceNodeIdResolver root_resolver(manifest);
    EXPECT_EQ(root_resolver.resolve(person.source).id, person.node.global_id);
    EXPECT_EQ(get(resolver, "Post", "A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11").source.value,
              "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
    EXPECT_THROW((void) get(resolver, "Person", "7"), std::invalid_argument);
    EXPECT_THROW((void) get(resolver, "Person", 999), std::invalid_argument);
}
TEST_F(RelationalAttachment, BootstrapIsDeterministicAcrossOrderAndBatchSize) {
    const auto first = attach("first");
    RelationalIdentityResolver a(first, mapping());
    std::swap(document["nodes"][0], document["nodes"][2]);
    sql("CREATE INDEX person_reverse_order ON " + quote_identifier(sql_schema) + ".person(id DESC); CLUSTER " +
        quote_identifier(sql_schema) + ".person USING person_reverse_order");
    const auto second =
            attach_relational_nodes(connection.get(), mapping(), {.output_dir = root / "second", .batch_size = 3});
    RelationalIdentityResolver b(second, mapping());
    EXPECT_EQ(get(a, "Person", 7).node.global_id, get(b, "Person", 7).node.global_id);
    EXPECT_EQ(get(a, "Item", "雪").node.global_id, get(b, "Item", "雪").node.global_id);
}
TEST_F(RelationalAttachment, RefreshPreservesIdsAndRetiresRemovedKeys) {
    const auto first = attach("first");
    RelationalIdentityResolver a(first, mapping());
    const auto original = get(a, "Person", 7).node;
    sql("DELETE FROM " + quote_identifier(sql_schema) + ".person WHERE id=7; INSERT INTO " +
        quote_identifier(sql_schema) + ".person VALUES(100)");
    const auto second = attach("second", first);
    RelationalIdentityResolver b(second, mapping());
    EXPECT_THROW((void) get(b, "Person", 7), std::invalid_argument);
    const std::array<NodeRef, 1> retired{original};
    EXPECT_THROW((void) b.reverse(retired), std::invalid_argument);
    EXPECT_EQ(get(a, "Person", -2).node.global_id, get(b, "Person", -2).node.global_id);
    EXPECT_NE(original.global_id, get(b, "Person", 100).node.global_id);
    sql("INSERT INTO " + quote_identifier(sql_schema) + ".person VALUES(7)");
    EXPECT_THROW((void) attach("reused", second), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(root / "reused"));
    EXPECT_EQ(get(a, "Person", 7).node.global_id, original.global_id);
}
TEST_F(RelationalAttachment, RejectsChangedMappingGenerationAndCorruptedCatalog) {
    const auto manifest = attach("first");
    RelationalIdentityResolver original(manifest, mapping());
    EXPECT_THROW((void) RelationalIdentityResolver(manifest, mapping(), "wrong-generation"), std::invalid_argument);
    document["nodes"][0]["namespace"] = "other";
    EXPECT_THROW((void) RelationalIdentityResolver(manifest, mapping()), std::invalid_argument);
    EXPECT_THROW((void) attach("mismatch", manifest), std::invalid_argument);
    document["nodes"][0]["namespace"] = "app";
    std::ofstream(root / "first/catalog.sqlite3", std::ios::binary | std::ios::app) << 'x';
    EXPECT_THROW((void) RelationalIdentityResolver(manifest, mapping()), std::invalid_argument);
}
TEST_F(RelationalAttachment, RejectsOverwriteAndCallerTransactionAndBoundsLookups) {
    const auto manifest = attach("first");
    EXPECT_THROW((void) attach("first"), std::invalid_argument);
    sql("BEGIN");
    EXPECT_THROW((void) attach("transaction"), std::invalid_argument);
    sql("ROLLBACK");
    RelationalIdentityResolver r(manifest, mapping());
    std::vector<NodeRef> too_many(RelationalIdentityResolver::max_batch_size + 1);
    EXPECT_THROW((void) r.reverse(too_many), std::invalid_argument);
    const std::array<NodeRef, 1> wrong{{{get(r, "Person", 7).node.global_id, schema.node_type_id("Item")}}};
    EXPECT_THROW((void) r.reverse(wrong), std::invalid_argument);
}

int main(int argc, char **argv) {
    if (!std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN"))
        return 77;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST_F(RelationalAttachment, AdoptsPreparedIngestionIdsAndOwnersWithoutReallocatingThem) {
    namespace ing = polygraph::ingestion;
    class Rows final : public ing::IGraphInputAdapter {
    public:
        void nodes(const ing::RecordConsumer &consume) override {
            consume({.type = "Person", .identity = domain::SourceIdentity{"app", "Person", json(std::int64_t{7})}});
            consume({.type = "Item", .identity = domain::SourceIdentity{"app", "Item", "7"}});
            consume({.type = "Post",
                     .identity = domain::SourceIdentity{"app", "Post", "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11"}});
        }
        void relationships(const ing::RecordConsumer &consume) override {
            consume({.type = "Owns",
                     .from_identity = domain::SourceIdentity{"app", "Person", json(std::int64_t{7})},
                     .to_identity = domain::SourceIdentity{"app", "Item", "7"}});
        }
        json metadata() const override { return json::object(); }
    };
    std::ofstream(root / "input") << "immutable input\n";
    ing::write_json_atomic(root / "placement.json", {{"storageBackends", {"Neo4J", "ArangoDB", "PostgreSQL"}},
                                                     {"defaultNodeSource", "PostgreSQL"},
                                                     {"nodeSources", {{"Person", "Neo4J"}}},
                                                     {"defaultRelationshipSource", "ArangoDB"}});
    ing::InputAdapterRegistry registry;
    registry.add({.name = "relational-seed",
                  .factory = [](const auto &, const auto &) { return std::make_unique<Rows>(); },
                  .input_files = [](const auto &o) { return std::vector<ing::fs::path>{o.input}; },
                  .id_policy = ing::InputIdPolicy::typed_external});
    (void) ing::prepare({.adapter = "relational-seed",
                         .input = root / "input",
                         .placement_config = root / "placement.json",
                         .output_dir = root / "prepared",
                         .input_adapters = &registry});
    const auto manifest = attach_relational_nodes(
            connection.get(), mapping(),
            {.output_dir = root / "attached", .source_manifest = root / "prepared/manifest.json"});
    RelationalIdentityResolver resolver(manifest, mapping());
    cli::SourceNodeIdResolver ingested(root / "prepared/manifest.json");
    const auto person = get(resolver, "Person", 7);
    EXPECT_EQ(person.node.global_id, ingested.resolve(person.source).id);
    EXPECT_EQ(person.owner, NEO4J);
    EXPECT_EQ(get(resolver, "Item", "7").owner, POSTGRESQL);
    const auto added = get(resolver, "Person", -2);
    EXPECT_GE(added.node.global_id & ((GlobalId{1} << 48) - 1), 4);
}

TEST_F(RelationalAttachment, CommandLineUsesExistingLogicalSchemaLoaderAndLibpqEnvironment) {
    ingestion::write_json_atomic(root / "mapping.json", document);
    ingestion::write_json_atomic(
            root / "schema.json",
            {{"nodeTypes", json::array({{{"name", "Person"}}, {{"name", "Item"}}, {{"name", "Post"}}})},
             {"relations", json::array()}});
    const auto out = root / "cli";
    const auto result = ingestion::run_process(
            {"env", std::string("PGHOST=") + PQhost(connection.get()),
             std::string("PGPORT=") + PQport(connection.get()), std::string("PGDATABASE=") + PQdb(connection.get()),
             std::string("PGUSER=") + PQuser(connection.get()), POLYGRAPH_ATTACH_POSTGRES, "--schema",
             (root / "schema.json").string(), "--mapping", (root / "mapping.json").string(), "--output-dir",
             out.string()},
            root / "cli-process");
    EXPECT_EQ(result.exit_code, 0);
    ASSERT_TRUE(std::filesystem::exists(out / "manifest.json"));
    // The public schema loader adds the same synthetic Proxy type as runtime startup.
    const auto runtime_schema = cli::GraphRuntimeBuilder::load_schema((root / "schema.json").string());
    RelationalIdentityResolver resolver(out / "manifest.json", parse_relational_mapping(document, *runtime_schema));
    EXPECT_EQ(get(resolver, "Person", 7).node.type, runtime_schema->node_type_id("Person"));
}
TEST_F(RelationalAttachment, AttachmentNeedsOnlyApplicationReadPrivileges) {
    const auto role = quote_identifier("reader_" + sql_schema);
    const auto ns = quote_identifier(sql_schema);
    sql("CREATE ROLE " + role + "; GRANT USAGE ON SCHEMA " + ns + " TO " + role +
        "; GRANT SELECT ON ALL TABLES IN SCHEMA " + ns + " TO " + role + "; SET ROLE " + role);
    EXPECT_NO_THROW((void) attach("reader"));
    sql("RESET ROLE; REVOKE ALL ON ALL TABLES IN SCHEMA " + ns + " FROM " + role + "; REVOKE ALL ON SCHEMA " + ns +
        " FROM " + role + "; DROP ROLE " + role);
}

TEST_F(RelationalAttachment, RejectsSameSizeCorruptionAndEscapingIndexPaths) {
    const auto manifest = attach("first");
    auto document = ingestion::read_json(manifest);
    const auto outside = root / "outside.sqlite3";
    std::filesystem::copy_file(root / "first/catalog.sqlite3", outside);
    document["index"]["path"] = "../outside.sqlite3";
    ingestion::write_json_atomic(manifest, document);
    EXPECT_THROW((void) RelationalIdentityResolver(manifest, mapping()), std::invalid_argument);
    document["index"]["path"] = "catalog.sqlite3";
    ingestion::write_json_atomic(manifest, document);
    std::fstream file(root / "first/catalog.sqlite3", std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(100);
    char byte{};
    file.read(&byte, 1);
    byte ^= 1;
    file.seekp(100);
    file.write(&byte, 1);
    file.close();
    try {
        (void) RelationalIdentityResolver(manifest, mapping());
        FAIL() << "accepted checksum corruption";
    } catch (const std::invalid_argument &e) {
        EXPECT_NE(std::string(e.what()).find("checksum"), std::string::npos);
    }
}
TEST_F(RelationalAttachment, StreamsTablesLargerThanLookupBatchAndPreservesLookupOrder) {
    sql("INSERT INTO " + quote_identifier(sql_schema) + ".person SELECT generate_series(100,5099)");
    const auto manifest =
            attach_relational_nodes(connection.get(), mapping(), {.output_dir = root / "large", .batch_size = 127});
    const auto metadata = ingestion::read_json(manifest);
    EXPECT_EQ(metadata["counts"]["nodes"], 5007);
    RelationalIdentityResolver resolver(manifest, mapping());
    const std::array<domain::SourceIdentity, 3> keys{
            {{"app", "Person", 5099}, {"app", "Item", "007"}, {"app", "Person", 100}}};
    const auto found = resolver.resolve(keys);
    ASSERT_EQ(found.size(), 3);
    EXPECT_EQ(found[0].source.value, 5099);
    EXPECT_EQ(found[1].source.value, "007");
    EXPECT_EQ(found[2].source.value, 100);
    const std::array<domain::SourceIdentity, 1> wrong_namespace{{{"wrong", "Person", 7}}};
    EXPECT_THROW((void) resolver.resolve(wrong_namespace), std::invalid_argument);
}

TEST_F(RelationalAttachment, FailedSnapshotRollsBackAndRemovesOnlyItsNewGeneration) {
    const auto first = attach("first");
    const auto original_table = document["nodes"][0]["table"];
    document["nodes"][0]["table"] = "missing";
    EXPECT_THROW((void) attach("failed"), std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(connection.get()), PQTRANS_IDLE);
    EXPECT_FALSE(std::filesystem::exists(root / "failed"));
    EXPECT_TRUE(std::filesystem::exists(first));
    document["nodes"][0]["table"] = original_table;
    EXPECT_NO_THROW((void) attach("retry"));
}
TEST_F(RelationalAttachment, TextKeysUseUtf8IndependentlyOfCallerClientEncoding) {
    sql("SET client_encoding='LATIN1'");
    const auto manifest = attach("latin1");
    EXPECT_EQ(std::string(PQparameterStatus(connection.get(), "client_encoding")), "LATIN1");
    sql("SET client_encoding='UTF8'");
    RelationalIdentityResolver resolver(manifest, mapping());
    EXPECT_EQ(get(resolver, "Item", "雪").source.value, "雪");
}

TEST_F(RelationalAttachment, RelationalHistoryCannotBeBypassedThroughIngestionSeedOption) {
    const auto first = attach("first");
    EXPECT_THROW((void) attach_relational_nodes(connection.get(), mapping(),
                                                {.output_dir = root / "seeded", .source_manifest = first}),
                 std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(root / "seeded"));
}

TEST_F(RelationalAttachment, LocksEveryTableBeforeTakingTheDataSnapshot) {
    ingestion::write_json_atomic(root / "mapping.json", document);
    ingestion::write_json_atomic(
            root / "schema.json",
            {{"nodeTypes", json::array({{{"name", "Person"}}, {{"name", "Item"}}, {{"name", "Post"}}})},
             {"relations", json::array()}});
    sql("BEGIN; LOCK TABLE " + quote_identifier(sql_schema) + ".post IN ACCESS EXCLUSIVE MODE");
    std::vector<std::string> arguments{"env",
                                       std::string("PGHOST=") + PQhost(connection.get()),
                                       std::string("PGPORT=") + PQport(connection.get()),
                                       std::string("PGDATABASE=") + PQdb(connection.get()),
                                       std::string("PGUSER=") + PQuser(connection.get()),
                                       POLYGRAPH_ATTACH_POSTGRES,
                                       "--schema",
                                       (root / "schema.json").string(),
                                       "--mapping",
                                       (root / "mapping.json").string(),
                                       "--output-dir",
                                       (root / "concurrent").string()};
    std::vector<char *> argv;
    for (auto &argument: arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t child{};
    ASSERT_EQ(posix_spawnp(&child, "env", nullptr, nullptr, argv.data(), environ), 0);
    bool waiting = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto query = "SELECT count(*) FROM pg_catalog.pg_locks WHERE relation='" + quote_identifier(sql_schema) +
                           ".post'::pg_catalog.regclass AND mode='AccessShareLock' AND NOT granted";
        std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), query.c_str()), PQclear);
        if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
            break;
        if (std::string_view(PQgetvalue(result.get(), 0, 0)) != "0") {
            waiting = true;
            break;
        }
    }
    if (waiting)
        sql("TRUNCATE " + quote_identifier(sql_schema) + ".post; INSERT INTO " + quote_identifier(sql_schema) +
            ".post VALUES('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11')");
    sql("COMMIT"); // Always release the blocker before waiting for the child.
    int status{};
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(waiting);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    RelationalIdentityResolver resolver(root / "concurrent/manifest.json", mapping());
    EXPECT_EQ(get(resolver, "Post", "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11").source.value,
              "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
}
