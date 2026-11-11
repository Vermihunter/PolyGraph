#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <libpq-fe.h>
#include <sqlite3.h>
#include <unistd.h>
#include "../src/ingestion/Native.hpp"
#include "data_access/adapters/PostgreSQL/RelationalForeignKeyReader.hpp"
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"
#include "data_access/adapters/PostgreSQL/RelationalRelationshipTableReader.hpp"
#include "domain/graph/LogicalGraphSchemaJson.hpp"
#include "ingestion/InputAdapterRegistry.hpp"
#include "ingestion/Process.hpp"
#include "ingestion/RelationalImport.hpp"
#include "utilities/BackendIds.hpp"

namespace ing = polygraph::ingestion;
using namespace polygraph;
namespace {
    class Rows final : public ing::IGraphInputAdapter {
        std::vector<ing::Record> rows_;
        std::vector<ing::Record> edges_;

    public:
        explicit Rows(std::vector<ing::Record> rows, std::vector<ing::Record> edges) :
            rows_(std::move(rows)), edges_(std::move(edges)) {}
        void nodes(const ing::RecordConsumer &c) override {
            for (const auto &r: rows_)
                c(r);
        }
        void relationships(const ing::RecordConsumer &c) override {
            for (const auto &r: edges_)
                c(r);
        }
        json metadata() const override { return json::object(); }
    };
    class RelationalIngestion : public ::testing::Test {
    protected:
        ing::fs::path root;
        std::string database;
        std::string limited_role;
        std::unique_ptr<PGconn, decltype(&PQfinish)> admin{nullptr, PQfinish}, db{nullptr, PQfinish};
        ing::InputAdapterRegistry registry;
        std::vector<ing::Record> rows, edges;
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
                          .factory = [this](const auto &, const auto &) { return std::make_unique<Rows>(rows, edges); },
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
        json prepare(unsigned max_record_mb = 8, bool partitioned = false) {
            ing::PrepareOptions options{.adapter = "relational-test",
                                        .input = root / "input.json",
                                        .placement_config = root / "placement.json",
                                        .output_dir = root / "prepared",
                                        .max_record_mb = max_record_mb,
                                        .input_adapters = &registry,
                                        .relational_schema = root / "schema.json",
                                        .relational_mapping = root / "mapping.json"};
            if (partitioned) {
                options.partition_file = root / "partitions.jsonl";
                options.partition_map = root / "partition-map.json";
            }
            return ing::prepare(options);
        }
        void execute() { ing::execute(root / "prepared/manifest.json", root / "targets.json", true, 1); }
        void remote_placement() {
            ing::write_json_atomic(root / "placement.json",
                                   {{"storageBackends", {"PostgreSQL", "Neo4J"}},
                                    {"nodeSources", {{"Person", "PostgreSQL"}, {"Post", "Neo4J"}}},
                                    {"defaultRelationshipSource", "PostgreSQL"}});
        }
        std::unique_ptr<ing::IImportExecutor> pg_executor(const json &prepared) {
            return ing::detail::postgres_relational_executor({.backend = ing::Backend::postgresql,
                                                              .root = root / "prepared",
                                                              .artifacts = prepared["backends"]["PostgreSQL"],
                                                              .target = targets["targets"]["PostgreSQL"]});
        }
        int metadata_count(const std::string &sql) {
            sqlite3 *raw = nullptr;
            const auto path = root / "catalog/ingestion.sqlite3";
            if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
                sqlite3_close(raw);
                throw std::runtime_error("Cannot read published ingestion metadata");
            }
            const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> index(raw, sqlite3_close);
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(index.get(), sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
                throw std::runtime_error("Cannot prepare published metadata query");
            const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> result(statement, sqlite3_finalize);
            if (sqlite3_step(result.get()) != SQLITE_ROW)
                throw std::runtime_error("Cannot read published metadata row");
            return sqlite3_column_int(result.get(), 0);
        }
        void foreign_keys(bool owner_to = false, bool nullable = true) {
            const auto column = owner_to ? "author_id" : "fav_post";
            sql(db.get(), std::string("ALTER TABLE app.") +
                                  (owner_to ? "post ADD COLUMN author_id bigint" : "person ADD COLUMN fav_post uuid") +
                                  (nullable ? "" : " NOT NULL") + " REFERENCES app." +
                                  (owner_to ? "person(id)" : "post(id)"));
            schema["relations"].push_back({{"name", "FAVORITE"}, {"from", "Person"}, {"to", "Post"}});
            mapping["relationships"].push_back({{"type", "FAVORITE"},
                                                {"kind", "foreignKey"},
                                                {"owner", owner_to ? "to" : "from"},
                                                {"column", column}});
            ing::write_json_atomic(root / "schema.json", schema);
            ing::write_json_atomic(root / "mapping.json", mapping);
            edges = {{.type = "FAVORITE",
                      .identity = ing::SourceIdentity{"app", "FAVORITE", "fav-1"},
                      .from_identity = rows[0].identity,
                      .to_identity = rows[2].identity}};
        }
        void relationships(bool keyed = true) {
            sql(db.get(), "CREATE TABLE app.likes(" + std::string(keyed ? "id bigint PRIMARY KEY," : "") +
                                  "person_id bigint NOT NULL REFERENCES app.person(id),post_id uuid NOT NULL "
                                  "REFERENCES app.post(id),"
                                  "weight smallint DEFAULT 3 CHECK(weight>=0),note text DEFAULT md5(random()::text))");
            schema["relations"].push_back({{"name", "LIKES"},
                                           {"from", "Person"},
                                           {"to", "Post"},
                                           {"properties", json::array({{{"name", "weight"}, {"type", "integer"}},
                                                                       {{"name", "note"}, {"type", "string"}}})}});
            json relation{{"type", "LIKES"},
                          {"kind", "table"},
                          {"schema", "app"},
                          {"table", "likes"},
                          {"fromColumn", "person_id"},
                          {"toColumn", "post_id"},
                          {"properties", {{"weight", "weight"}, {"note", "note"}}}};
            if (keyed) {
                relation["primaryKey"] = "id";
                relation["keyCodec"] = "int64";
            }
            mapping["relationships"].push_back(relation);
            ing::write_json_atomic(root / "schema.json", schema);
            ing::write_json_atomic(root / "mapping.json", mapping);
            edges = {{.type = "LIKES",
                      .properties = {{"weight", 1}, {"note", "雪,\"quoted\"\n"}},
                      .identity = ing::SourceIdentity{"app", "LIKES", std::int64_t{91}},
                      .from_identity = rows[0].identity,
                      .to_identity = rows[2].identity},
                     {.type = "LIKES",
                      .properties = {{"weight", nullptr}, {"note", nullptr}},
                      .identity = ing::SourceIdentity{"app", "LIKES", std::int64_t{92}},
                      .from_identity = rows[0].identity,
                      .to_identity = rows[2].identity},
                     {.type = "LIKES",
                      .properties = json::object(),
                      .identity = ing::SourceIdentity{"app", "LIKES", std::int64_t{93}},
                      .from_identity = rows[1].identity,
                      .to_identity = rows[2].identity}};
        }
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
TEST_F(RelationalIngestion, ImportsMappedRelationshipsAfterNodesWithNativeKeysAndAttributes) {
    relationships();
    const auto prepared = prepare();
    EXPECT_EQ(prepared["backends"]["PostgreSQL"]["counts"]["edges"], 3);
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "3");
    EXPECT_EQ(scalar("SELECT person_id FROM app.likes WHERE id=91"), "7");
    EXPECT_EQ(scalar("SELECT post_id FROM app.likes WHERE id=91"), uuid);
    EXPECT_EQ(scalar("SELECT note FROM app.likes WHERE id=91"), "雪,\"quoted\"\n");
    EXPECT_EQ(scalar("SELECT weight IS NULL AND note IS NULL FROM app.likes WHERE id=92"), "t");
    EXPECT_EQ(scalar("SELECT weight=3 AND note IS NOT NULL FROM app.likes WHERE id=93"), "t");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
    EXPECT_TRUE(ing::fs::exists(root / "catalog/ingestion.sqlite3"));
}
TEST_F(RelationalIngestion, KeylessParallelRowsAndVolatileDefaultsVerifyAsAMultiset) {
    relationships(false);
    for (unsigned i = 0; i < 270; ++i)
        edges.push_back({.type = "LIKES",
                         .properties = {{"weight", 1}, {"note", "same"}},
                         .identity = ing::SourceIdentity{"app", "LIKES", "parallel-" + std::to_string(i)},
                         .from_identity = rows[0].identity,
                         .to_identity = rows[2].identity});
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes WHERE note='same'"), "270");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
    sql(db.get(), "UPDATE app.likes SET weight=2 WHERE ctid=(SELECT ctid FROM app.likes WHERE note='same' LIMIT 1)");
    EXPECT_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"), std::exception);
}
TEST_F(RelationalIngestion, RelationshipConstraintFailureRollsBackNodesEdgesAndCatalog) {
    relationships();
    edges[1].properties["weight"] = -1;
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
    EXPECT_FALSE(ing::fs::exists(root / "catalog/manifest.json"));
}
TEST_F(RelationalIngestion, MissingRelationshipEndpointFailsOffline) {
    relationships();
    edges[0].from_identity = ing::SourceIdentity{"app", "Person", std::int64_t{999}};
    EXPECT_THROW(prepare(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, RelationshipOverflowFailsBeforeAnyBackendMutation) {
    relationships();
    edges[0].properties["weight"] = 70000;
    prepare();
    try {
        execute();
        FAIL() << "Native relationship integer overflow accepted";
    } catch (const std::exception &e) {
        EXPECT_NE(std::string(e.what()).find("phase=preflight"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(ing::fs::exists(root / "prepared/execution-state.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, RelationshipPublicationRecoveryChecksCommittedEdgesBeforePublishing) {
    relationships(false);
    prepare();
    ing::fs::create_directory(root / "catalog");
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "3");
    ing::fs::remove(root / "catalog");
    EXPECT_NO_THROW(
            (void) ing::recover_postgres_relational_identity(root / "prepared/manifest.json", root / "targets.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "3");
    EXPECT_TRUE(ing::fs::exists(root / "catalog/manifest.json"));
}
TEST_F(RelationalIngestion, CrossBackendReferencesRetainProxyOwnerPartitionsAndCanonicalEdgeIds) {
    relationships();
    remote_placement();
    sql(db.get(), "INSERT INTO app.post VALUES('00000000-0000-0000-0000-00000000000a','existing remote reference')");
    std::ofstream assignments(root / "partitions.jsonl");
    for (unsigned i = 0; i < rows.size(); ++i)
        assignments << json{{"key", ing::source_identity_key(*rows[i].identity)}, {"partition", i}}.dump() << '\n';
    assignments.close();
    ing::write_json_atomic(root / "partition-map.json", {{"0", "PostgreSQL"}, {"1", "PostgreSQL"}, {"2", "Neo4J"}});
    const auto prepared = prepare(8, true);
    EXPECT_EQ(prepared["backends"]["PostgreSQL"]["counts"]["proxies"], 1);
    EXPECT_EQ(prepared["backends"]["Neo4J"]["counts"]["edges"], 3);
    auto executor = pg_executor(prepared);
    executor->preflight();
    (void) executor->run();
    (void) executor->verify();
    EXPECT_EQ(scalar("SELECT content FROM app.post"), "existing remote reference");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "2");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "3");
    EXPECT_EQ(metadata_count("SELECT count(*) FROM proxies WHERE backend='PostgreSQL'"), 1);
    EXPECT_EQ(metadata_count("SELECT count(*) FROM source_edges s JOIN edges e ON s.id=e.id WHERE "
                             "json_extract(e.properties,'$.is_partition_cut')=1"),
              3);
    EXPECT_EQ(metadata_count("SELECT count(*) FROM replicas WHERE backend='Neo4J'"), 3);
    auto logical = domain::logical_schema_from_json(schema);
    auto map = postgres::parse_relational_mapping(mapping, *logical);
    postgres::RelationalIdentityResolver resolver(root / "catalog/manifest.json", map);
    const std::array<domain::SourceIdentity, 1> identities{*rows[2].identity};
    const auto post = resolver.resolve(identities);
    EXPECT_EQ(post[0].owner, NEO4J);
    EXPECT_EQ(post[0].node.global_id, ing::make_id(ing::Backend::neo4j, 2));
    postgres::RelationalRelationshipTableReader reader(map, resolver);
    unsigned incoming = 0;
    const std::array<NodeRef, 1> frontier{post[0].node};
    reader.stream(db.get(), frontier, {.direction = postgres::RelationshipLookupDirection::incoming}, [&](auto batch) {
        incoming += batch.size();
        for (const auto &row: batch)
            EXPECT_EQ(row.to.owner, NEO4J);
    });
    EXPECT_EQ(incoming, 3);
}
TEST_F(RelationalIngestion, RemoteOnlyEndpointFailsPreflightWithoutInventingApplicationRows) {
    relationships();
    remote_placement();
    auto executor = pg_executor(prepare());
    EXPECT_THROW(executor->preflight(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "0");
}
TEST_F(RelationalIngestion, KeylessSelfLoopsAndReverseTraversalPreserveLogicalOrientation) {
    relationships(false);
    sql(db.get(), "CREATE TABLE app.knows(a bigint REFERENCES app.person(id),b bigint REFERENCES app.person(id))");
    schema["relations"].push_back({{"name", "KNOWS"}, {"from", "Person"}, {"to", "Person"}});
    mapping["relationships"].push_back({{"type", "KNOWS"},
                                        {"kind", "table"},
                                        {"schema", "app"},
                                        {"table", "knows"},
                                        {"fromColumn", "a"},
                                        {"toColumn", "b"}});
    ing::write_json_atomic(root / "schema.json", schema);
    ing::write_json_atomic(root / "mapping.json", mapping);
    edges = {{.type = "KNOWS",
              .identity = ing::SourceIdentity{"app", "KNOWS", "loop"},
              .from_identity = rows[0].identity,
              .to_identity = rows[0].identity},
             {.type = "KNOWS",
              .identity = ing::SourceIdentity{"app", "KNOWS", "reverse"},
              .from_identity = rows[1].identity,
              .to_identity = rows[0].identity}};
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.knows WHERE a=7 AND b=7"), "1");
    auto logical = domain::logical_schema_from_json(schema);
    auto map = postgres::parse_relational_mapping(mapping, *logical);
    postgres::RelationalIdentityResolver resolver(root / "catalog/manifest.json", map);
    const std::array<domain::SourceIdentity, 1> identities{*rows[0].identity};
    const auto person = resolver.resolve(identities);
    const std::array<NodeRef, 1> frontier{person[0].node};
    postgres::RelationalRelationshipTableReader reader(map, resolver);
    unsigned incoming = 0;
    reader.stream(db.get(), frontier, {.direction = postgres::RelationshipLookupDirection::incoming},
                  [&](auto batch) { incoming += batch.size(); });
    EXPECT_EQ(incoming, 2);
}
TEST_F(RelationalIngestion, ExistingMappedRelationshipRowsFailBeforeNodeCopy) {
    relationships();
    sql(db.get(), "ALTER TABLE app.likes DROP CONSTRAINT likes_person_id_fkey,DROP CONSTRAINT likes_post_id_fkey;"
                  "INSERT INTO app.likes(id,person_id,post_id) VALUES(999,7,'00000000-0000-0000-0000-00000000000a')");
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_FALSE(ing::fs::exists(root / "prepared/execution-state.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT id FROM app.likes"), "999");
}
TEST_F(RelationalIngestion, RelationshipTriggersCannotSilentlyChangeExplicitSourceValues) {
    relationships(false);
    sql(db.get(),
        "CREATE FUNCTION app.change_weight() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.weight=99; RETURN NEW; "
        "END $$;"
        "CREATE TRIGGER change_weight BEFORE INSERT ON app.likes FOR EACH ROW EXECUTE FUNCTION app.change_weight()");
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, RecoveryRejectsChangedRealizedRelationshipDefaults) {
    relationships(false);
    prepare();
    ing::fs::create_directory(root / "catalog");
    EXPECT_THROW(execute(), std::exception);
    ing::fs::remove(root / "catalog");
    sql(db.get(), "UPDATE app.likes SET note='changed' WHERE person_id=8");
    EXPECT_THROW(
            (void) ing::recover_postgres_relational_identity(root / "prepared/manifest.json", root / "targets.json"),
            std::exception);
    EXPECT_FALSE(ing::fs::exists(root / "catalog/manifest.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "3");
}
TEST_F(RelationalIngestion, PublishedCanonicalEdgeMetadataIsCheckedForCorruption) {
    relationships();
    prepare();
    execute();
    EXPECT_EQ(metadata_count("SELECT count(*) FROM source_edges"), 3);
    std::ofstream(root / "catalog/ingestion.sqlite3", std::ios::app) << 'x';
    EXPECT_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "3");
}
TEST_F(RelationalIngestion, EndpointColumnCanAlsoBeTheMappedRelationshipPrimaryKey) {
    relationships();
    sql(db.get(), "ALTER TABLE app.likes DROP COLUMN id; ALTER TABLE app.likes ADD PRIMARY KEY(person_id)");
    mapping["relationships"][0]["primaryKey"] = "person_id";
    ing::write_json_atomic(root / "mapping.json", mapping);
    edges = {edges[0], edges[2]};
    edges[0].identity->value = std::int64_t{7};
    edges[1].identity->value = std::int64_t{8};
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes WHERE person_id IN(7,8)"), "2");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
}
TEST_F(RelationalIngestion, RelationshipTemporaryStoragePrivilegeIsCheckedBeforeAnyCopy) {
    relationships();
    limited_role = database + "_loader";
    sql(db.get(), "CREATE ROLE \"" + limited_role + "\" LOGIN; GRANT USAGE ON SCHEMA app TO \"" + limited_role +
                          "\"; GRANT SELECT,INSERT,UPDATE ON ALL TABLES IN SCHEMA app TO \"" + limited_role +
                          "\"; REVOKE TEMPORARY ON DATABASE \"" + database + "\" FROM PUBLIC");
    targets["targets"]["PostgreSQL"]["username"] = limited_role;
    ing::write_json_atomic(root / "targets.json", targets);
    prepare();
    try {
        execute();
        FAIL() << "Missing TEMPORARY privilege accepted";
    } catch (const std::exception &e) {
        EXPECT_NE(std::string(e.what()).find("phase=preflight"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(ing::fs::exists(root / "prepared/execution-state.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, DeferredRelationshipTriggerChangesRollBackTheEntireImport) {
    relationships(false);
    sql(db.get(), "CREATE FUNCTION app.defer_weight() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
                  "UPDATE app.likes SET weight=99 WHERE person_id=NEW.person_id; RETURN NULL; END $$;"
                  "CREATE CONSTRAINT TRIGGER defer_weight AFTER INSERT ON app.likes DEFERRABLE INITIALLY DEFERRED "
                  "FOR EACH ROW EXECUTE FUNCTION app.defer_weight()");
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "0");
    EXPECT_FALSE(ing::fs::exists(root / "catalog/manifest.json"));
}
TEST_F(RelationalIngestion, AuditTriggersCanUpdateUnmappedColumnsAcrossRelationshipBatches) {
    relationships(false);
    sql(db.get(),
        "ALTER TABLE app.likes ADD COLUMN touched_at timestamptz;"
        "CREATE FUNCTION app.touch_likes() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
        "UPDATE app.likes SET touched_at=clock_timestamp(); RETURN NULL; END $$;"
        "CREATE TRIGGER touch_likes AFTER INSERT ON app.likes FOR EACH STATEMENT EXECUTE FUNCTION app.touch_likes()");
    for (unsigned i = 0; i < 270; ++i)
        edges.push_back({.type = "LIKES",
                         .properties = {{"weight", 1}, {"note", "same"}},
                         .identity = ing::SourceIdentity{"app", "LIKES", "audit-" + std::to_string(i)},
                         .from_identity = rows[0].identity,
                         .to_identity = rows[2].identity});
    prepare();
    ASSERT_NO_THROW(execute());
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes WHERE touched_at IS NOT NULL"), "273");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
}
TEST_F(RelationalIngestion, ForeignKeyImportResolvesNativeTargetsAndNullableAbsence) {
    foreign_keys();
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT fav_post FROM app.person WHERE id=7"), uuid);
    EXPECT_EQ(scalar("SELECT fav_post IS NULL FROM app.person WHERE id=8"), "t");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
    auto logical = domain::logical_schema_from_json(schema);
    auto map = postgres::parse_relational_mapping(mapping, *logical);
    postgres::RelationalIdentityResolver resolver(root / "catalog/manifest.json", map);
    const std::array<domain::SourceIdentity, 1> ids{*rows[2].identity};
    const auto post = resolver.resolve(ids);
    const std::array<NodeRef, 1> frontier{post[0].node};
    postgres::RelationalForeignKeyReader reader(map, resolver);
    unsigned incoming = 0;
    reader.stream(db.get(), frontier, {.direction = postgres::RelationshipLookupDirection::incoming},
                  [&](auto batch) { incoming += batch.size(); });
    EXPECT_EQ(incoming, 1);
}
TEST_F(RelationalIngestion, ForeignKeyOwnerToUsesReferencedFromKey) {
    foreign_keys(true);
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT author_id FROM app.post"), "7");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
}
TEST_F(RelationalIngestion, ForeignKeyDuplicateAssignmentsAreIdempotent) {
    foreign_keys();
    auto duplicate = edges[0];
    duplicate.identity->value = "fav-duplicate";
    edges.push_back(duplicate);
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person WHERE fav_post IS NOT NULL"), "1");
    EXPECT_EQ(metadata_count("SELECT count(*) FROM source_edges"), 2);
}
TEST_F(RelationalIngestion, ForeignKeyConflictingTargetsAreRejectedOffline) {
    foreign_keys();
    rows.push_back(
            {.type = "Post", .identity = ing::SourceIdentity{"app", "Post", "00000000-0000-0000-0000-00000000000b"}});
    auto conflict = edges[0];
    conflict.identity->value = "conflict";
    conflict.to_identity = rows.back().identity;
    edges.push_back(conflict);
    EXPECT_THROW(prepare(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, ForeignKeyNonnullableAcyclicDependenciesInsertReferencedTypesFirst) {
    foreign_keys(false, false);
    auto second = edges[0];
    second.identity->value = "fav-2";
    second.from_identity = rows[1].identity;
    edges.push_back(second);
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person WHERE fav_post IS NOT NULL"), "2");
}
TEST_F(RelationalIngestion, ForeignKeyRequiredAbsenceFailsPreflight) {
    foreign_keys(false, false);
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_FALSE(ing::fs::exists(root / "prepared/execution-state.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
}
TEST_F(RelationalIngestion, ForeignKeyNullableNoEdgeOverridesNativeDefault) {
    foreign_keys();
    sql(db.get(), "ALTER TABLE app.person ALTER COLUMN fav_post SET DEFAULT '00000000-0000-0000-0000-00000000000a'");
    edges.clear();
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person WHERE fav_post IS NULL"), "2");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
}
TEST_F(RelationalIngestion, ForeignKeyRecoveryChecksCommittedAssignmentsWithoutReimporting) {
    foreign_keys();
    prepare();
    ing::fs::create_directory(root / "catalog");
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT fav_post FROM app.person WHERE id=7"), uuid);
    ing::fs::remove(root / "catalog");
    EXPECT_NO_THROW(
            (void) ing::recover_postgres_relational_identity(root / "prepared/manifest.json", root / "targets.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "2");
    sql(db.get(), "UPDATE app.person SET fav_post=NULL WHERE id=7");
    EXPECT_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"), std::exception);
}
TEST_F(RelationalIngestion, ForeignKeyNonnullableCyclesFailBeforeAnyWrites) {
    foreign_keys(false, false);
    sql(db.get(), "ALTER TABLE app.post ADD COLUMN creator_id bigint NOT NULL REFERENCES app.person(id) DEFERRABLE "
                  "INITIALLY DEFERRED");
    schema["relations"].push_back({{"name", "CREATOR"}, {"from", "Post"}, {"to", "Person"}});
    mapping["relationships"].push_back(
            {{"type", "CREATOR"}, {"kind", "foreignKey"}, {"owner", "from"}, {"column", "creator_id"}});
    ing::write_json_atomic(root / "schema.json", schema);
    ing::write_json_atomic(root / "mapping.json", mapping);
    auto second = edges[0];
    second.identity->value = "fav-2";
    second.from_identity = rows[1].identity;
    edges.push_back(second);
    edges.push_back({.type = "CREATOR",
                     .identity = ing::SourceIdentity{"app", "CREATOR", "creator"},
                     .from_identity = rows[2].identity,
                     .to_identity = rows[0].identity});
    prepare();
    try {
        execute();
        FAIL() << "Unsupported nonnullable cycle accepted";
    } catch (const std::exception &e) {
        EXPECT_NE(std::string(e.what()).find("phase=preflight"), std::string::npos);
    }
    EXPECT_FALSE(ing::fs::exists(root / "prepared/execution-state.json"));
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
}
TEST_F(RelationalIngestion, ForeignKeyNullableCyclesAndSelfLoopsAreAssignedAfterInsertion) {
    foreign_keys();
    sql(db.get(), "ALTER TABLE app.person ADD COLUMN manager_id bigint REFERENCES app.person(id)");
    schema["relations"].push_back({{"name", "MANAGER"}, {"from", "Person"}, {"to", "Person"}});
    mapping["relationships"].push_back(
            {{"type", "MANAGER"}, {"kind", "foreignKey"}, {"owner", "from"}, {"column", "manager_id"}});
    ing::write_json_atomic(root / "schema.json", schema);
    ing::write_json_atomic(root / "mapping.json", mapping);
    rows.push_back({.type = "Person", .identity = ing::SourceIdentity{"app", "Person", std::int64_t{9}}});
    for (const auto &[from, to]: std::vector<std::pair<unsigned, unsigned>>{{0, 1}, {1, 0}, {3, 3}})
        edges.push_back({.type = "MANAGER",
                         .identity = ing::SourceIdentity{"app", "MANAGER", std::int64_t{from}},
                         .from_identity = rows[from].identity,
                         .to_identity = rows[to].identity});
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT manager_id FROM app.person WHERE id=7"), "8");
    EXPECT_EQ(scalar("SELECT manager_id FROM app.person WHERE id=8"), "7");
    EXPECT_EQ(scalar("SELECT manager_id FROM app.person WHERE id=9"), "9");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
}
TEST_F(RelationalIngestion, ForeignKeyRemoteTargetUsesExistingLocalReferenceAndPreservesOwner) {
    foreign_keys();
    remote_placement();
    sql(db.get(), "INSERT INTO app.post VALUES('00000000-0000-0000-0000-00000000000a','remote reference')");
    auto executor = pg_executor(prepare());
    executor->preflight();
    (void) executor->run();
    (void) executor->verify();
    EXPECT_EQ(scalar("SELECT fav_post FROM app.person WHERE id=7"), uuid);
    EXPECT_EQ(scalar("SELECT content FROM app.post"), "remote reference");
    EXPECT_EQ(metadata_count("SELECT count(*) FROM proxies WHERE backend='PostgreSQL'"), 1);
    EXPECT_EQ(metadata_count("SELECT count(*) FROM source_edges"), 1);
}
TEST_F(RelationalIngestion, ForeignKeyRemoteOnlyTargetIsRejectedBeforeWrites) {
    foreign_keys();
    remote_placement();
    auto executor = pg_executor(prepare());
    EXPECT_THROW(executor->preflight(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
}
TEST_F(RelationalIngestion, ForeignKeyPreexistingReferenceOwnerIsNeverUpdated) {
    foreign_keys(true);
    remote_placement();
    sql(db.get(), "INSERT INTO app.post(id,content) VALUES('00000000-0000-0000-0000-00000000000a','preserve')");
    EXPECT_THROW(prepare(), std::exception);
    EXPECT_EQ(scalar("SELECT author_id IS NULL FROM app.post"), "t");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, ForeignKeyUpdateConstraintFailureRollsBackAllInsertedRows) {
    foreign_keys();
    sql(db.get(), "ALTER TABLE app.person ADD CHECK(fav_post IS NULL OR id<>7)");
    prepare();
    EXPECT_THROW(execute(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
    EXPECT_FALSE(ing::fs::exists(root / "catalog/manifest.json"));
}
TEST_F(RelationalIngestion, ForeignKeyAssignmentsStayCorrectAcrossBoundedBatches) {
    foreign_keys();
    for (std::int64_t i = 10; i < 280; ++i) {
        rows.push_back({.type = "Person", .identity = ing::SourceIdentity{"app", "Person", i}});
        edges.push_back({.type = "FAVORITE",
                         .identity = ing::SourceIdentity{"app", "FAVORITE", i},
                         .from_identity = rows.back().identity,
                         .to_identity = rows[2].identity});
    }
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person WHERE fav_post IS NOT NULL"), "271");
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
}
TEST_F(RelationalIngestion, ForeignKeyUnresolvedSourceReferenceIsRejectedOffline) {
    foreign_keys();
    edges[0].to_identity = ing::SourceIdentity{"app", "Post", "00000000-0000-0000-0000-00000000000b"};
    EXPECT_THROW(prepare(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.person"), "0");
}
TEST_F(RelationalIngestion, ForeignKeyMappedPropertyCannotContradictRelationshipAssignment) {
    foreign_keys(true);
    schema["nodeTypes"][1]["properties"].push_back({{"name", "author"}, {"type", "integer"}});
    mapping["nodes"][1]["properties"]["author"] = "author_id";
    ing::write_json_atomic(root / "schema.json", schema);
    ing::write_json_atomic(root / "mapping.json", mapping);
    rows[2].properties["author"] = 8;
    EXPECT_THROW(prepare(), std::exception);
    EXPECT_EQ(scalar("SELECT count(*) FROM app.post"), "0");
}
TEST_F(RelationalIngestion, ForeignKeyAndJunctionRelationshipsShareOneAtomicImport) {
    relationships();
    const auto junction = edges;
    foreign_keys();
    edges.insert(edges.end(), junction.begin(), junction.end());
    prepare();
    execute();
    EXPECT_EQ(scalar("SELECT count(*) FROM app.likes"), "3");
    EXPECT_EQ(scalar("SELECT fav_post FROM app.person WHERE id=7"), uuid);
    EXPECT_EQ(metadata_count("SELECT count(*) FROM source_edges"), 4);
    EXPECT_NO_THROW((void) ing::verify_live(root / "prepared/manifest.json", root / "targets.json"));
}
int main(int argc, char **argv) {
    if (!std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN"))
        return 77;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
