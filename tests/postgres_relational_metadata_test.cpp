#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include "data_access/adapters/PostgreSQL/RelationalMappingValidator.hpp"

using namespace polygraph;
using namespace polygraph::postgres;

namespace {
    class RelationalMetadata : public ::testing::Test {
    protected:
        std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
        domain::LogicalGraphSchema logical;
        json document;
        void sql(const std::string &query) {
            std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), query.c_str()), PQclear);
            ASSERT_TRUE(result);
            ASSERT_TRUE(PQresultStatus(result.get()) == PGRES_COMMAND_OK ||
                        PQresultStatus(result.get()) == PGRES_TUPLES_OK)
                    << PQresultErrorMessage(result.get());
        }
        void SetUp() override {
            const char *dsn = std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN");
            if (!dsn)
                GTEST_SKIP() << "Set POLYGRAPH_RELATIONAL_TEST_DSN to an owned disposable PostgreSQL database";
            connection.reset(PQconnectdb(dsn));
            ASSERT_EQ(PQstatus(connection.get()), CONNECTION_OK) << PQerrorMessage(connection.get());
            sql("BEGIN");
            sql(R"(CREATE SCHEMA "relational µ";
                CREATE TABLE "relational µ".person(id bigint PRIMARY KEY, name text NOT NULL);
                CREATE TABLE "relational µ".post(id uuid PRIMARY KEY, author bigint REFERENCES "relational µ".person(id));
                CREATE TABLE "relational µ".likes(person_id bigint, post_id uuid, weight integer);
                CREATE TABLE "relational µ".sentinel(value text);
                INSERT INTO "relational µ".sentinel VALUES ('untouched'))");
            const auto p = logical.add_node_type("Person", {{"name", domain::PropertyType::string}});
            const auto post = logical.add_node_type("Post");
            logical.add_edge_type("Likes", p, post, true, {{"weight", domain::PropertyType::integer}});
            logical.add_edge_type("Author", post, p);
            document = {
                    {"format", "polygraph.postgresql-relational"},
                    {"version", 1},
                    {"nodes", json::array({{{"type", "Person"},
                                            {"schema", "relational µ"},
                                            {"table", "person"},
                                            {"primaryKey", "id"},
                                            {"keyCodec", "int64"},
                                            {"namespace", "app"},
                                            {"properties", {{"name", "name"}}}},
                                           {{"type", "Post"},
                                            {"schema", "relational µ"},
                                            {"table", "post"},
                                            {"primaryKey", "id"},
                                            {"keyCodec", "uuid"},
                                            {"namespace", "app"}}})},
                    {"relationships",
                     json::array(
                             {{{"type", "Likes"},
                               {"kind", "table"},
                               {"schema", "relational µ"},
                               {"table", "likes"},
                               {"fromColumn", "person_id"},
                               {"toColumn", "post_id"},
                               {"properties", {{"weight", "weight"}}}},
                              {{"type", "Author"}, {"kind", "foreignKey"}, {"owner", "from"}, {"column", "author"}}})}};
        }
        void TearDown() override {
            if (connection && PQstatus(connection.get()) == CONNECTION_OK)
                sql("ROLLBACK");
        }
        ValidatedRelationalMapping validate() {
            return RelationalMappingValidator{}.validate(connection.get(), parse_relational_mapping(document, logical));
        }
        void rejects(std::string_view message) {
            try {
                (void) validate();
                FAIL() << "accepted invalid metadata";
            } catch (const std::invalid_argument &e) {
                EXPECT_NE(std::string(e.what()).find(message), std::string::npos) << e.what();
            }
        }
    };
} // namespace
TEST_F(RelationalMetadata, ValidatesTypedTablesAndNullableForeignKeyReadOnly) {
    sql("SET LOCAL search_path=pg_catalog; SET LOCAL transaction_read_only=on");
    const auto validated = validate();
    EXPECT_EQ(validated.mapping().nodes().size(), 2);
    EXPECT_EQ(validated.tables().size(), 3);
    const auto &post = validated.tables()[1];
    EXPECT_GT(post.oid, 0);
    EXPECT_TRUE(post.columns.at("author").nullable);
    sql("SELECT value FROM \"relational µ\".sentinel WHERE value='untouched'");
}
TEST_F(RelationalMetadata, RejectsMissingTableAndColumnWithContext) {
    document["nodes"][0]["table"] = "missing";
    rejects("Person");
    document["nodes"][0]["table"] = "person";
    document["nodes"][0]["properties"]["name"] = "missing";
    rejects("missing");
}
TEST_F(RelationalMetadata, RejectsWrongKeyAndPropertyTypes) {
    document["nodes"][0]["keyCodec"] = "string";
    rejects("keyCodec");
    document["nodes"][0]["keyCodec"] = "int64";
    document["nodes"][0]["properties"]["name"] = "id";
    rejects("property");
}
TEST_F(RelationalMetadata, RejectsNonPrimaryAndCompositeKeys) {
    sql("ALTER TABLE \"relational µ\".person DROP CONSTRAINT person_pkey CASCADE");
    rejects("primary key");
    sql("ALTER TABLE \"relational µ\".person ADD PRIMARY KEY(id,name)");
    rejects("primary key");
}
TEST_F(RelationalMetadata, RejectsUndeclaredForeignKeyAndWrongEndpointType) {
    sql("ALTER TABLE \"relational µ\".post DROP CONSTRAINT post_author_fkey");
    rejects("foreign key");
    document["relationships"].erase(1);
    document["relationships"][0]["toColumn"] = "person_id";
    rejects("endpoint");
}
TEST_F(RelationalMetadata, SupportsForeignKeyOnLogicalTarget) {
    document["relationships"].erase(0);
    document["relationships"][0]["type"] = "Authored";
    document["relationships"][0]["owner"] = "to";
    logical.add_edge_type("Authored", logical.node_type_id("Person"), logical.node_type_id("Post"));
    EXPECT_NO_THROW((void) validate());
}
TEST_F(RelationalMetadata, NeverInterpretsMappingIdentifiersAsSql) {
    const std::string name = "p\"; DROP TABLE sentinel;--";
    sql("ALTER TABLE \"relational µ\".person RENAME TO " + quote_identifier(name));
    document["nodes"][0]["table"] = name;
    EXPECT_NO_THROW((void) validate());
    document["nodes"][0]["table"] = "absent'; DROP SCHEMA public CASCADE;--";
    rejects("table");
    sql("SELECT * FROM \"relational µ\".sentinel");
}
TEST_F(RelationalMetadata, ChecksColumnPrivilegesAndSchemaUsage) {
    sql("CREATE ROLE polygraph_issue13_reader; GRANT USAGE ON SCHEMA \"relational µ\" TO polygraph_issue13_reader;");
    sql("GRANT SELECT(id) ON \"relational µ\".person TO polygraph_issue13_reader; SET LOCAL ROLE "
        "polygraph_issue13_reader");
    rejects("SELECT");
    sql("RESET ROLE; GRANT SELECT ON ALL TABLES IN SCHEMA \"relational µ\" TO polygraph_issue13_reader; SET LOCAL ROLE "
        "polygraph_issue13_reader");
    EXPECT_NO_THROW((void) validate());
    sql("RESET ROLE; REVOKE USAGE ON SCHEMA \"relational µ\" FROM polygraph_issue13_reader; SET LOCAL ROLE "
        "polygraph_issue13_reader");
    rejects("USAGE");
}
TEST_F(RelationalMetadata, RejectsUnsupportedNativePropertyTypesAndViews) {
    sql("ALTER TABLE \"relational µ\".person ADD COLUMN tags text[]");
    document["nodes"][0]["properties"]["name"] = "tags";
    rejects("property");
    document["nodes"][0]["properties"]["name"] = "name";
    sql("CREATE VIEW \"relational µ\".person_view AS SELECT * FROM \"relational µ\".person");
    document["nodes"][0]["table"] = "person_view";
    rejects("table");
}
TEST_F(RelationalMetadata, RejectsUnvalidatedForeignKey) {
    sql("ALTER TABLE \"relational µ\".post DROP CONSTRAINT post_author_fkey; ALTER TABLE \"relational µ\".post ADD "
        "FOREIGN KEY(author) REFERENCES \"relational µ\".person(id) NOT VALID");
    rejects("validated foreign key");
}
TEST_F(RelationalMetadata, ValidatesOptionalRelationshipPrimaryKey) {
    sql("ALTER TABLE \"relational µ\".likes ADD COLUMN edge_key text PRIMARY KEY");
    document["relationships"][0]["primaryKey"] = "edge_key";
    document["relationships"][0]["keyCodec"] = "string";
    EXPECT_NO_THROW((void) validate());
    document["relationships"][0]["primaryKey"] = "person_id";
    document["relationships"][0]["keyCodec"] = "int64";
    rejects("primary key");
}
TEST_F(RelationalMetadata, RejectsActiveRowLevelSecurity) {
    sql("CREATE ROLE polygraph_issue13_rls; GRANT USAGE ON SCHEMA \"relational µ\" TO polygraph_issue13_rls; GRANT "
        "SELECT ON ALL TABLES IN SCHEMA \"relational µ\" TO polygraph_issue13_rls; ALTER TABLE \"relational µ\".person "
        "ENABLE ROW LEVEL SECURITY; SET LOCAL ROLE polygraph_issue13_rls");
    rejects("row-level security");
}

int main(int argc, char **argv) {
    if (!std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN")) {
        std::cerr << "Set POLYGRAPH_RELATIONAL_TEST_DSN to an owned disposable PostgreSQL database\n";
        return 77;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST_F(RelationalMetadata, CatalogValidationIgnoresShadowedOperatorsAndTypes) {
    sql(R"(CREATE SCHEMA relational_shadow;
        CREATE FUNCTION relational_shadow.equal_name(name,name) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT false';
        CREATE OPERATOR relational_shadow.= (LEFTARG=name, RIGHTARG=name, FUNCTION=relational_shadow.equal_name);
        CREATE DOMAIN relational_shadow.oid AS pg_catalog.text;
        CREATE DOMAIN relational_shadow.text AS pg_catalog.text;
        CREATE DOMAIN relational_shadow.int2 AS pg_catalog.int2;
        CREATE FUNCTION relational_shadow.equal_oid(pg_catalog.oid,pg_catalog.oid) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT false';
        CREATE OPERATOR relational_shadow.= (LEFTARG=pg_catalog.oid, RIGHTARG=pg_catalog.oid, FUNCTION=relational_shadow.equal_oid);
        CREATE FUNCTION relational_shadow.equal_array(pg_catalog.int2[],pg_catalog.int2[]) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT false';
        CREATE OPERATOR relational_shadow.= (LEFTARG=pg_catalog.int2[], RIGHTARG=pg_catalog.int2[], FUNCTION=relational_shadow.equal_array);
        SET LOCAL search_path=relational_shadow,pg_catalog)");
    EXPECT_NO_THROW((void) validate());
}
TEST_F(RelationalMetadata, RejectsTraditionalInheritanceWithoutGlobalKeyConstraints) {
    sql("CREATE TABLE \"relational µ\".person_child () INHERITS (\"relational µ\".person)");
    rejects("inheritance");
}

TEST_F(RelationalMetadata, AcceptsPartitionedTablesWithGlobalPrimaryKeys) {
    sql(R"(DROP TABLE "relational µ".person CASCADE;
        CREATE TABLE "relational µ".person(id bigint PRIMARY KEY,name text NOT NULL) PARTITION BY RANGE(id);
        CREATE TABLE "relational µ".person_partition PARTITION OF "relational µ".person FOR VALUES FROM (0) TO (100);
        ALTER TABLE "relational µ".post ADD FOREIGN KEY(author) REFERENCES "relational µ".person(id))");
    EXPECT_NO_THROW((void) validate());
}
