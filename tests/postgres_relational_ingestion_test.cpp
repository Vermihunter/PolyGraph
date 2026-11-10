#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <libpq-fe.h>
#include <sqlite3.h>
#include <unistd.h>
#include "../src/ingestion/Native.hpp"
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"
#include "domain/graph/LogicalGraphSchemaJson.hpp"
#include "ingestion/InputAdapterRegistry.hpp"
#include "ingestion/Process.hpp"
#include "ingestion/RelationalImport.hpp"

namespace ing = polygraph::ingestion;
using namespace polygraph;
namespace {
    class Rows final : public ing::IGraphInputAdapter {
        std::vector<ing::Record> rows_;

    public:
        explicit Rows(std::vector<ing::Record> rows) : rows_(std::move(rows)) {}
        void nodes(const ing::RecordConsumer &c) override {
            for (const auto &r: rows_)
                c(r);
        }
        void relationships(const ing::RecordConsumer &) override {}
        json metadata() const override { return json::object(); }
    };
    class RelationalIngestion : public ::testing::Test {
    protected:
        ing::fs::path root;
        std::string database;
        std::string limited_role;
        std::unique_ptr<PGconn, decltype(&PQfinish)> admin{nullptr, PQfinish}, db{nullptr, PQfinish};
        ing::InputAdapterRegistry registry;
        std::vector<ing::Record> rows;
        json schema, mapping, targets;
        const std::string uuid = "00000000-0000-0000-0000-00000000000a";
        void sql(PGconn *c, const std::string &s) {
            std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(c, s.c_str()), PQclear);
            if (!r || (PQresultStatus(r.get()) != PGRES_COMMAND_OK && PQresultStatus(r.get()) != PGRES_TUPLES_OK))
                throw std::runtime_error(r ? PQresultErrorMessage(r.get()) : PQerrorMessage(c));
        }
        std::string scalar(const std::string &s) {
            std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(db.get(), s.c_str()), PQclear);
            if (!r || PQresultStatus(r.get()) != PGRES_TUPLES_OK)
                throw std::runtime_error("query failed");
            return PQgetvalue(r.get(), 0, 0);
        }
        void SetUp() override {
            auto pattern = (ing::fs::temp_directory_path() / "polygraph-relimport-XXXXXX").string();
            auto path = mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            root = path;
            database = "relimport_" + root.filename().string().substr(20);
            admin.reset(PQconnectdb(std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN")));
            ASSERT_EQ(PQstatus(admin.get()), CONNECTION_OK) << PQerrorMessage(admin.get());
            sql(admin.get(), "CREATE DATABASE \"" + database + "\"");
            const char *keys[] = {"host", "port", "user", "dbname", nullptr};
            const char *values[] = {PQhost(admin.get()), PQport(admin.get()), PQuser(admin.get()), database.c_str(),
                                    nullptr};
            db.reset(PQconnectdbParams(keys, values, 0));
            ASSERT_EQ(PQstatus(db.get()), CONNECTION_OK);
            sql(db.get(),
                "CREATE SCHEMA app; CREATE TABLE app.person(id bigint PRIMARY KEY,name text DEFAULT 'unknown',score "
                "double precision,total bigint,active boolean NOT NULL DEFAULT true,born date DEFAULT "
                "'2020-01-01',CHECK(name IS DISTINCT FROM 'bad')); CREATE TABLE app.post(id uuid PRIMARY KEY,content "
                "varchar(200)); CREATE TABLE app.sentinel(value text); INSERT INTO app.sentinel VALUES('untouched')");
            schema = {{"nodeTypes",
                       json::array({{{"name", "Person"},
                                     {"properties", json::array({{{"name", "name"}, {"type", "string"}},
                                                                 {{"name", "score"}, {"type", "float"}},
                                                                 {{"name", "total"}, {"type", "integer"}},
                                                                 {{"name", "active"}, {"type", "boolean"}},
                                                                 {{"name", "born"}, {"type", "date"}}})}},
                                    {{"name", "Post"},
                                     {"properties", json::array({{{"name", "content"}, {"type", "string"}}})}}})},
                      {"relations", json::array()}};
            mapping = {{"format", "polygraph.postgresql-relational"},
                       {"version", 1},
                       {"relationships", json::array()},
                       {"nodes", json::array({{{"type", "Person"},
                                               {"schema", "app"},
                                               {"table", "person"},
                                               {"primaryKey", "id"},
                                               {"namespace", "app"},
                                               {"keyCodec", "int64"},
                                               {"properties",
                                                {{"name", "name"},
                                                 {"score", "score"},
                                                 {"total", "total"},
                                                 {"active", "active"},
                                                 {"born", "born"}}}},
                                              {{"type", "Post"},
                                               {"schema", "app"},
                                               {"table", "post"},
                                               {"primaryKey", "id"},
                                               {"namespace", "app"},
                                               {"keyCodec", "uuid"},
                                               {"properties", {{"content", "content"}}}}})}};
            ing::write_json_atomic(root / "schema.json", schema);
            ing::write_json_atomic(root / "mapping.json", mapping);
            ing::write_json_atomic(root / "input.json", {{"fixture", 1}});
            ing::write_json_atomic(root / "placement.json", {{"storageBackends", {"PostgreSQL"}},
                                                             {"defaultNodeSource", "PostgreSQL"},
                                                             {"defaultRelationshipSource", "PostgreSQL"}});
            rows = {{.type = "Person",
                     .properties = {{"name", "雪"},
                                    {"score", 1.25},
                                    {"total", INT64_MAX},
                                    {"active", false},
                                    {"born", "2000-01-02"}},
                     .identity = ing::SourceIdentity{"app", "Person", std::int64_t{7}}},
                    {.type = "Person",
                     .properties = {{"name", nullptr}, {"score", nullptr}, {"total", INT64_MIN}},
                     .identity = ing::SourceIdentity{"app", "Person", std::int64_t{8}}},
                    {.type = "Post",
                     .properties = {{"content", "line,\"quoted\"\n雪"}},
                     .identity = ing::SourceIdentity{"app", "Post", uuid}}};
            registry.add({.name = "relational-test",
                          .factory = [this](const auto &, const auto &) { return std::make_unique<Rows>(rows); },
                          .input_files = [](const auto &o) { return std::vector<ing::fs::path>{o.input}; },
                          .id_policy = ing::InputIdPolicy::typed_external});
            targets = {{"targets",
                        {{"PostgreSQL",
                          {{"database", database},
                           {"host", PQhost(admin.get())},
                           {"port", std::stoi(PQport(admin.get()))},
                           {"username", PQuser(admin.get())},
                           {"relationalMapping", (root / "mapping.json").string()},
                           {"relationalIdentityOutput", (root / "catalog").string()}}}}}};
            ing::write_json_atomic(root / "targets.json", targets);
        }
        void TearDown() override {
            db.reset();
            if (admin && PQstatus(admin.get()) == CONNECTION_OK && !database.empty())
                sql(admin.get(), "DROP DATABASE IF EXISTS \"" + database + "\" WITH (FORCE)");
            if (admin && PQstatus(admin.get()) == CONNECTION_OK && !limited_role.empty())
                sql(admin.get(), "DROP ROLE \"" + limited_role + "\"");
            if (!root.empty())
                ing::fs::remove_all(root);
        }
        json prepare(unsigned max_record_mb = 8) {
            return ing::prepare({.adapter = "relational-test",
                                 .input = root / "input.json",
                                 .placement_config = root / "placement.json",
                                 .output_dir = root / "prepared",
                                 .max_record_mb = max_record_mb,
                                 .input_adapters = &registry,
                                 .relational_schema = root / "schema.json",
                                 .relational_mapping = root / "mapping.json"});
        }
        void execute() { ing::execute(root / "prepared/manifest.json", root / "targets.json", true, 1); }
    };
} // namespace
TEST_F(RelationalIngestion, PrepareImportVerifyPreserveTypedValuesDefaultsAndPrimaryKeys) {
    const auto prepared = prepare();
    EXPECT_EQ(prepared["backends"]["PostgreSQL"]["importer"], "libpq-relational-copy");
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "2");
    EXPECT_EQ(scalar("SELECT name FROM app.person WHERE id=7"), "雪");
    EXPECT_EQ(scalar("SELECT total FROM app.person WHERE id=7"), "9223372036854775807");
    EXPECT_EQ(scalar("SELECT active FROM app.person WHERE id=8"), "t");
    EXPECT_EQ(scalar("SELECT name IS NULL FROM app.person WHERE id=8"), "t");
    EXPECT_EQ(scalar("SELECT born FROM app.person WHERE id=8"), "2020-01-01");
    EXPECT_EQ(scalar("SELECT value FROM app.sentinel"), "untouched");
    EXPECT_EQ(scalar("SELECT to_regclass('public.polygraph_nodes') IS NULL"), "t");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
    auto logical = domain::logical_schema_from_json(schema);
    auto map = postgres::parse_relational_mapping(mapping, *logical);
    postgres::RelationalIdentityResolver resolver(root / "catalog/manifest.json", map);
    const std::array<domain::SourceIdentity, 1> keys{{{"app", "Person", std::int64_t{7}}}};
    const auto resolved = resolver.resolve(keys);
    EXPECT_EQ(resolved[0].node.global_id, ing::make_id(ing::Backend::postgresql, 0));
}
TEST_F(RelationalIngestion, TableConstraintFailureRollsBackEveryNodeTable) {
    rows[1].properties["name"] = "bad";
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
    EXPECT_FALSE(ing::fs::exists(root / "catalog/manifest.json"));
}
TEST_F(RelationalIngestion, RefusesPopulatedMappedTableButIgnoresUnrelatedTables) {
    prepare();
    sql(db.get(), "INSERT INTO app.person(id) VALUES(999)");
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "1");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
}
TEST_F(RelationalIngestion, PublicationRecoveryDoesNotReimportCommittedData) {
    prepare();
    ing::fs::create_directory(root / "catalog");
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "2");
    EXPECT_FALSE(ing::fs::exists(root / "catalog/manifest.json"));
    ing::fs::remove(root / "catalog");
    EXPECT_NO_THROW(
            (void) ing::recover_postgres_relational_identity(root / "prepared/manifest.json", root / "targets.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "2");
    EXPECT_TRUE(ing::fs::exists(root / "catalog/manifest.json"));
}
TEST_F(RelationalIngestion, RecoveryRebuildsAnInterruptedUnpublishedCatalog) {
    prepare();
    ing::fs::create_directory(root / "catalog");
    EXPECT_THROW(execute(), std::exception);
    ing::fs::remove(root / "catalog");
    const auto journal = ing::read_json(root / "prepared/relational-postgresql-publication.json");
    const auto staged = root / (".polygraph-" + journal.at("binding").get<std::string>() + ".staged");
    ing::fs::create_directory(staged);
    std::ofstream(staged / "index.sqlite") << "interrupted catalog build";
    EXPECT_NO_THROW(
            (void) ing::recover_postgres_relational_identity(root / "prepared/manifest.json", root / "targets.json"));
    EXPECT_TRUE(ing::fs::exists(root / "catalog/manifest.json"));
    EXPECT_FALSE(ing::fs::exists(staged));
}
TEST_F(RelationalIngestion, RecoveryRejectsModifiedCommittedData) {
    prepare();
    ing::fs::create_directory(root / "catalog");
    EXPECT_THROW(execute(), std::exception);
    ing::fs::remove(root / "catalog");
    sql(db.get(), "UPDATE app.person SET total=0 WHERE id=7");
    EXPECT_THROW(
            (void) ing::recover_postgres_relational_identity(root / "prepared/manifest.json", root / "targets.json"),
            std::exception);
    EXPECT_FALSE(ing::fs::exists(root / "catalog/manifest.json"));
    EXPECT_EQ(scalar("SELECT total FROM app.person WHERE id=7"), "0");
}
TEST_F(RelationalIngestion, MissingRequiredColumnsFailPreflight) {
    sql(db.get(), "ALTER TABLE app.person ADD COLUMN required text NOT NULL");
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, NativeIntegerOverflowFailsBeforeAnyCopy) {
    sql(db.get(), "ALTER TABLE app.person ALTER COLUMN total TYPE smallint");
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
    EXPECT_FALSE(ing::fs::exists(root / "prepared/relational-postgresql-publication.json"));
}
TEST_F(RelationalIngestion, RejectsAutomaticPrimaryKeysBeforeMutation) {
    sql(db.get(), "ALTER TABLE app.person ALTER COLUMN id ADD GENERATED BY DEFAULT AS IDENTITY");
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, MissingWriteLockPrivilegesFailBeforeExecutionStarts) {
    limited_role = database + "_loader";
    sql(db.get(), "CREATE ROLE \"" + limited_role + "\" LOGIN; GRANT USAGE ON SCHEMA app TO \"" + limited_role +
                          "\"; GRANT SELECT,INSERT ON ALL TABLES IN SCHEMA app TO \"" + limited_role + "\"");
    targets["targets"]["PostgreSQL"]["username"] = limited_role;
    ing::write_json_atomic(root / "targets.json", targets);
    prepare();
    try {
        execute();
        FAIL() << "Import role without write-lock permission accepted";
    } catch (const std::exception &e) {
        EXPECT_NE(std::string(e.what()).find("phase=preflight"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(ing::fs::exists(root / "prepared/execution-state.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, OmittedDefaultsAndExplicitNullAndCsvSentinelsRemainDistinct) {
    rows[0].properties.erase("name");
    rows.push_back({.type = "Post",
                    .properties = {{"content", "\\N"}},
                    .identity = ing::SourceIdentity{"app", "Post", "00000000-0000-0000-0000-00000000000b"}});
    rows.push_back({.type = "Post",
                    .properties = {{"content", ""}},
                    .identity = ing::SourceIdentity{"app", "Post", "00000000-0000-0000-0000-00000000000c"}});
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT name FROM app.person WHERE id=7"), "unknown");
    EXPECT_EQ(scalar("SELECT name IS NULL FROM app.person WHERE id=8"), "t");
    EXPECT_EQ(scalar("SELECT content FROM app.post WHERE id='00000000-0000-0000-0000-00000000000b'"), "\\N");
    EXPECT_EQ(scalar("SELECT content='' AND content IS NOT NULL FROM app.post WHERE "
                     "id='00000000-0000-0000-0000-00000000000c'"),
              "t");
}
TEST_F(RelationalIngestion, TargetMappingDriftAndSkippedVerificationAreRejected) {
    prepare();
    mapping["nodes"][0]["properties"].erase("score");
    ing::write_json_atomic(root / "mapping.json", mapping);
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    mapping["nodes"][0]["properties"]["score"] = "score";
    ing::write_json_atomic(root / "mapping.json", mapping);
    EXPECT_THROW(ing::execute(root / "prepared/manifest.json", root / "targets.json", true, 1, {},
                              ing::VerificationMode::none),
                 std::exception);
}
TEST_F(RelationalIngestion, RejectsUnsignedOverflowDuringOfflinePreparation) {
    rows[0].properties["total"] = std::uint64_t{9223372036854775808ULL};
    EXPECT_THROW(prepare(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, StrictVerificationRetainsConfiguredLargeRowLimit) {
    for (unsigned i = 0; i < 9; ++i) {
        const auto name = "large" + std::to_string(i);
        sql(db.get(), "ALTER TABLE app.person ADD COLUMN " + name + " text");
        schema["nodeTypes"][0]["properties"].push_back({{"name", name}, {"type", "string"}});
        mapping["nodes"][0]["properties"][name] = name;
        rows[0].properties[name] = std::string(1024 * 1024, 'x');
    }
    ing::write_json_atomic(root / "schema.json", schema);
    ing::write_json_atomic(root / "mapping.json", mapping);
    prepare(12);
    EXPECT_NO_THROW((void) ing::verify(root / "prepared/manifest.json", true));
    ASSERT_NO_THROW(execute());
    EXPECT_EQ(scalar("SELECT length(large8) FROM app.person WHERE id=7"), "1048576");
}
TEST_F(RelationalIngestion, CommandLinePreparesImportsAndRecoversTypedSourceJsonl) {
    std::ofstream input(root / "source.jsonl");
    for (const auto &r: rows) {
        const auto &id = *r.identity;
        input << json{{"kind", "node"},
                      {"type", r.type},
                      {"properties", r.properties},
                      {"identity",
                       {{"namespace", id.name_space},
                        {"type", id.type},
                        {"idType", id.value.is_string() ? "string" : "int64"},
                        {"id", id.value}}}}
                         .dump()
              << '\n';
    }
    input.close();
    const auto prepared = ing::run_process(
            {POLYGRAPH_BULK_IMPORT, "prepare", "--adapter", "source-jsonl", "--input", (root / "source.jsonl").string(),
             "--placement-config", (root / "placement.json").string(), "--output-dir", (root / "prepared").string(),
             "--relational-schema", (root / "schema.json").string(), "--relational-mapping",
             (root / "mapping.json").string()},
            root / "cli-prepare");
    ASSERT_EQ(prepared.exit_code, 0) << prepared.stderr_text;
    ing::write_json_atomic(root / "backends.json", {{"backends", json::array({{{"kind", "PostgreSQL"}}})}});
    ing::write_json_atomic(
            root / "domain.json",
            {{"format", "polygraph.domain"},
             {"version", 1},
             {"id", "relational-cli"},
             {"paths",
              {{"schema", "schema.json"},
               {"data", "source.jsonl"},
               {"backendConfig", "backends.json"},
               {"placementConfig", "placement.json"},
               {"ingestionArtifacts", "graph-prepared"}}},
             {"ingestion", {{"adapter", "source-jsonl"}, {"options", {{"relational-mapping", "mapping.json"}}}}}});
    const auto graph_prepared = ing::run_process(
            {POLYGRAPH_BULK_IMPORT, "import", "--graph-manifest", (root / "domain.json").string()}, root / "cli-graph");
    ASSERT_EQ(graph_prepared.exit_code, 0) << graph_prepared.stderr_text;
    EXPECT_EQ(ing::read_json(root / "graph-prepared/manifest.json")["backends"]["PostgreSQL"]["importer"],
              "libpq-relational-copy");
    ing::fs::create_directory(root / "catalog");
    const auto imported = ing::run_process({POLYGRAPH_BULK_IMPORT, "execute", "--manifest",
                                            (root / "prepared/manifest.json").string(), "--target-config",
                                            (root / "targets.json").string(), "--confirm-create"},
                                           root / "cli-execute");
    EXPECT_NE(imported.exit_code, 0);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "2");
    ing::fs::remove(root / "catalog");
    const auto recovered = ing::run_process({POLYGRAPH_BULK_IMPORT, "recover-relational", "--manifest",
                                             (root / "prepared/manifest.json").string(), "--target-config",
                                             (root / "targets.json").string()},
                                            root / "cli-recover");
    EXPECT_EQ(recovered.exit_code, 0) << recovered.stderr_text;
    EXPECT_TRUE(ing::fs::exists(root / "catalog/manifest.json"));
}
TEST_F(RelationalIngestion, MixedPlacementAndPartitionMetadataStayOutsideApplicationColumns) {
    ing::write_json_atomic(root / "placement.json", {{"storageBackends", {"Neo4J", "PostgreSQL"}},
                                                     {"nodeSources", {{"Person", "PostgreSQL"}, {"Post", "Neo4J"}}},
                                                     {"defaultRelationshipSource", "PostgreSQL"}});
    std::ofstream assignments(root / "partitions.jsonl");
    for (std::size_t i = 0; i < rows.size(); ++i)
        assignments << json{{"key", ing::source_identity_key(*rows[i].identity)}, {"partition", i}}.dump() << '\n';
    assignments.close();
    ing::write_json_atomic(root / "partition-map.json", {{"0", "PostgreSQL"}, {"1", "PostgreSQL"}, {"2", "Neo4J"}});
    const auto prepared = ing::prepare({.adapter = "relational-test",
                                        .input = root / "input.json",
                                        .placement_config = root / "placement.json",
                                        .output_dir = root / "prepared",
                                        .partition_file = root / "partitions.jsonl",
                                        .partition_map = root / "partition-map.json",
                                        .input_adapters = &registry,
                                        .relational_schema = root / "schema.json",
                                        .relational_mapping = root / "mapping.json"});
    EXPECT_EQ(prepared["backends"]["PostgreSQL"]["counts"]["nodes"], 2);
    EXPECT_EQ(prepared["backends"]["Neo4J"]["counts"]["nodes"], 1);
    EXPECT_EQ(prepared["backends"]["PostgreSQL"]["counts"]["proxies"], 0);
    auto executor = ing::detail::postgres_relational_executor({.backend = ing::Backend::postgresql,
                                                               .root = root / "prepared",
                                                               .artifacts = prepared["backends"]["PostgreSQL"],
                                                               .target = targets["targets"]["PostgreSQL"]});
    executor->preflight();
    (void) executor->run();
    (void) executor->verify();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "2");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM information_schema.columns WHERE table_schema='app' AND "
                     "column_name='partition_id'"),
              "0");
    sqlite3 *raw = nullptr;
    const auto path = root / "prepared" / prepared["index"]["path"].get<std::string>();
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> index(raw, sqlite3_close);
    sqlite3_stmt *statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(index.get(),
                                 "SELECT count(*) FROM nodes WHERE owner='PostgreSQL' AND "
                                 "json_extract(properties,'$.partition_id') IN(0,1)",
                                 -1, &statement, nullptr),
              SQLITE_OK);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> result(statement, sqlite3_finalize);
    ASSERT_EQ(sqlite3_step(result.get()), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(result.get(), 0), 2);
}
TEST_F(RelationalIngestion, BoundedBatchesPreserveTextKeysAndGeneratedColumns) {
    sql(db.get(), "ALTER TABLE app.person ALTER COLUMN id TYPE text USING id::text; ALTER TABLE app.person ADD COLUMN "
                  "display text GENERATED ALWAYS AS (upper(name)) STORED");
    mapping["nodes"][0]["keyCodec"] = "string";
    mapping["nodes"][0]["properties"]["display"] = "display";
    schema["nodeTypes"][0]["properties"].push_back({{"name", "display"}, {"type", "string"}});
    ing::write_json_atomic(root / "schema.json", schema);
    ing::write_json_atomic(root / "mapping.json", mapping);
    rows.clear();
    for (unsigned i = 0; i < 301; ++i)
        rows.push_back({.type = "Person",
                        .properties = {{"name", "valid"}},
                        .identity = ing::SourceIdentity{"app", "Person", "key,\"雪" + std::to_string(i)}});
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person WHERE display='VALID'"), "301");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
}
int main(int argc, char **argv) {
    if (!std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN"))
        return 77;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
