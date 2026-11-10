#include <algorithm>
#include <cstdlib>
#include <gtest/gtest.h>
#include <unistd.h>
#include "data_access/adapters/PostgreSQL/PostgresRelationalNodeReader.hpp"
#include "data_access/adapters/PostgreSQL/RelationalAttachment.hpp"
#include "data_model/Value.hpp"

using namespace polygraph;
using namespace polygraph::postgres;
namespace {
    class RelationalNodeReads : public ::testing::Test {
    protected:
        std::filesystem::path root;
        std::string ns, other;
        domain::LogicalGraphSchema schema;
        json document;
        std::unique_ptr<PGconn, decltype(&PQfinish)> db{nullptr, PQfinish};
        std::unique_ptr<RelationalIdentityResolver> identities;
        TypeId person{}, post{}, comment{};
        const std::string uuid = "00000000-0000-0000-0000-00000000000a";
        std::string table(const char *name) { return quote_identifier(ns) + "." + quote_identifier(name); }
        void sql(const std::string &text) {
            std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(db.get(), text.c_str()), PQclear);
            if (!r || (PQresultStatus(r.get()) != PGRES_COMMAND_OK && PQresultStatus(r.get()) != PGRES_TUPLES_OK))
                throw std::runtime_error(r ? PQresultErrorMessage(r.get()) : PQerrorMessage(db.get()));
        }
        RelationalMapping mapping() { return parse_relational_mapping(document, schema); }
        void attach(const char *name) {
            const auto path = attach_relational_nodes(db.get(), mapping(), {.output_dir = root / name});
            identities = std::make_unique<RelationalIdentityResolver>(path, mapping());
        }
        void SetUp() override {
            auto pattern = (std::filesystem::temp_directory_path() / "polygraph-node-XXXXXX").string();
            const auto path = mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            root = path;
            ns = "node_" + root.filename().string().substr(15);
            other = ns + "_other";
            db.reset(PQconnectdb(std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN")));
            ASSERT_EQ(PQstatus(db.get()), CONNECTION_OK) << PQerrorMessage(db.get());
            sql("CREATE SCHEMA " + quote_identifier(ns) + "; CREATE SCHEMA " + quote_identifier(other) +
                "; CREATE TABLE " + table("person") +
                "(id bigint PRIMARY KEY,name text,age smallint,total bigint,active boolean,score float8,born "
                "date,created timestamptz,local_time timestamp,private_data text); CREATE TABLE " +
                quote_identifier(other) + ".post(id uuid PRIMARY KEY,content varchar(200)); CREATE TABLE " +
                table("comment") + "(id text PRIMARY KEY,content text)");
            sql("INSERT INTO " + table("person") +
                " VALUES(7,'雪',42,9223372036854775807,true,1.2345678901234567,'2000-01-02','2026-11-10 "
                "10:00:00+02','2026-11-10 "
                "10:00:00','private'),(-2,NULL,NULL,-9223372036854775808,false,NULL,NULL,NULL,NULL,NULL); INSERT "
                "INTO " +
                quote_identifier(other) + ".post VALUES('" + uuid + "','typed post'); INSERT INTO " + table("comment") +
                " VALUES('Alice','comment'),('雪',NULL)");
            person = schema.add_node_type("Person", {{"name", domain::PropertyType::string},
                                                     {"age", domain::PropertyType::integer},
                                                     {"total", domain::PropertyType::integer},
                                                     {"active", domain::PropertyType::boolean},
                                                     {"score", domain::PropertyType::_float},
                                                     {"born", domain::PropertyType::date},
                                                     {"created", domain::PropertyType::datetime},
                                                     {"local", domain::PropertyType::datetime}});
            post = schema.add_node_type("Post", {{"content", domain::PropertyType::string}});
            comment = schema.add_node_type("Comment", {{"content", domain::PropertyType::string}});
            document = {{"format", "polygraph.postgresql-relational"},
                        {"version", 1},
                        {"relationships", json::array()},
                        {"nodes", json::array({{{"type", "Person"},
                                                {"schema", ns},
                                                {"table", "person"},
                                                {"primaryKey", "id"},
                                                {"namespace", "app"},
                                                {"keyCodec", "int64"},
                                                {"properties",
                                                 {{"name", "name"},
                                                  {"age", "age"},
                                                  {"total", "total"},
                                                  {"active", "active"},
                                                  {"score", "score"},
                                                  {"born", "born"},
                                                  {"created", "created"},
                                                  {"local", "local_time"}}}},
                                               {{"type", "Post"},
                                                {"schema", other},
                                                {"table", "post"},
                                                {"primaryKey", "id"},
                                                {"namespace", "app"},
                                                {"keyCodec", "uuid"},
                                                {"properties", {{"content", "content"}}}},
                                               {{"type", "Comment"},
                                                {"schema", ns},
                                                {"table", "comment"},
                                                {"primaryKey", "id"},
                                                {"namespace", "app"},
                                                {"keyCodec", "string"},
                                                {"properties", {{"content", "content"}}}}})}};
            attach("initial");
        }
        void TearDown() override {
            if (db && PQstatus(db.get()) == CONNECTION_OK) {
                if (PQtransactionStatus(db.get()) != PQTRANS_IDLE)
                    sql("ROLLBACK");
                sql("RESET ROLE; DROP SCHEMA IF EXISTS " + quote_identifier(ns) + " CASCADE; DROP SCHEMA IF EXISTS " +
                    quote_identifier(other) + " CASCADE; DROP ROLE IF EXISTS " + quote_identifier("reader_" + ns));
            }
            if (!root.empty())
                std::filesystem::remove_all(root);
        }
        NodeRef node(const char *type, json key) {
            const std::array<domain::SourceIdentity, 1> source{{{"app", type, std::move(key)}}};
            return identities->resolve(source)[0].node;
        }
        std::vector<record_t> read(std::vector<NodeRef> input) {
            return PostgresRelationalNodeReader(mapping(), *identities).read_nodes(db.get(), input);
        }
        const Node &value(const record_t &record) {
            EXPECT_EQ(record->fields.size(), 1);
            return record->fields[0].as<Node>();
        }
    };
} // namespace
TEST_F(RelationalNodeReads, ReadsTypedColumnsIntoExistingNodeRecordModel) {
    const auto source = node("Person", 7);
    const auto records = read({source});
    ASSERT_EQ(records.size(), 1);
    const auto &n = value(records[0]);
    EXPECT_EQ(n.global_id, source.global_id);
    EXPECT_EQ(n.type, person);
    EXPECT_EQ(n.labels, std::vector<std::string>{"Person"});
    EXPECT_EQ(n.properties.at("name")->as<std::string>(), "雪");
    EXPECT_EQ(n.properties.at("age")->as<std::int64_t>(), 42);
    EXPECT_EQ(n.properties.at("total")->as<std::int64_t>(), INT64_MAX);
    EXPECT_TRUE(n.properties.at("active")->as<bool>());
    EXPECT_DOUBLE_EQ(n.properties.at("score")->as<double>(), 1.2345678901234567);
    EXPECT_EQ(n.properties.at("born")->as<std::string>(), "2000-01-02");
    EXPECT_EQ(n.properties.at("created")->as<std::string>(), "2026-11-10T08:00:00Z");
    EXPECT_EQ(n.properties.at("local")->as<std::string>(), "2026-11-10T10:00:00");
    EXPECT_FALSE(n.properties.contains("private_data"));
}
TEST_F(RelationalNodeReads, PreservesNullsAndSignedIntegerLimits) {
    const auto records = read({node("Person", -2)});
    const auto &n = value(records[0]);
    EXPECT_TRUE(n.properties.at("name")->is_null());
    EXPECT_TRUE(n.properties.at("age")->is_null());
    EXPECT_EQ(n.properties.at("total")->as<std::int64_t>(), INT64_MIN);
    EXPECT_FALSE(n.properties.at("active")->as<bool>());
}
TEST_F(RelationalNodeReads, MixedSchemasReturnRequestedOrderAndDuplicateRecords) {
    const auto p = node("Person", 7), c = node("Comment", "Alice"), u = node("Post", uuid);
    const auto records = read({u, p, c, p});
    ASSERT_EQ(records.size(), 4);
    EXPECT_EQ(value(records[0]).global_id, u.global_id);
    EXPECT_EQ(value(records[1]).global_id, p.global_id);
    EXPECT_EQ(value(records[2]).global_id, c.global_id);
    EXPECT_EQ(value(records[3]).global_id, p.global_id);
    EXPECT_EQ(value(records[0]).properties.at("content")->as<std::string>(), "typed post");
}
TEST_F(RelationalNodeReads, RestoresSessionSettingsAndDoesNotLoseFloatingPrecision) {
    sql("SET client_encoding='LATIN1'; SET DateStyle='SQL,DMY'; SET TimeZone='Europe/Bratislava'; SET "
        "extra_float_digits=-15");
    const auto records = read({node("Person", 7)});
    EXPECT_EQ(value(records[0]).properties.at("name")->as<std::string>(), "雪");
    EXPECT_DOUBLE_EQ(value(records[0]).properties.at("score")->as<double>(), 1.2345678901234567);
    EXPECT_EQ(std::string(PQparameterStatus(db.get(), "client_encoding")), "LATIN1");
    EXPECT_EQ(std::string(PQparameterStatus(db.get(), "TimeZone")), "Europe/Bratislava");
}
TEST_F(RelationalNodeReads, MissingLiveRowsFailAndConnectionRemainsReusable) {
    const auto p = node("Person", 7);
    sql("DELETE FROM " + table("person") + " WHERE id=7");
    EXPECT_THROW((void) read({p}), std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
    EXPECT_EQ(read({node("Person", -2)}).size(), 1);
}
TEST_F(RelationalNodeReads, RejectsNonFiniteAttributesInsteadOfReturningNull) {
    sql("UPDATE " + table("person") + " SET score='NaN' WHERE id=7");
    try {
        (void) read({node("Person", 7)});
        FAIL() << "accepted nonfinite property";
    } catch (const std::invalid_argument &error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Person"), std::string::npos);
        EXPECT_NE(message.find("score"), std::string::npos);
        EXPECT_NE(message.find(ns), std::string::npos);
    }
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
}
TEST_F(RelationalNodeReads, RefusesCallerTransactionsAndOversizedRequests) {
    sql("BEGIN");
    EXPECT_THROW((void) read({node("Person", 7)}), std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_INTRANS);
    sql("ROLLBACK");
    EXPECT_TRUE(read({}).empty());
    EXPECT_THROW((void) read(std::vector<NodeRef>(4097, node("Person", 7))), std::invalid_argument);
}
int main(int argc, char **argv) {
    if (!std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN"))
        return 77;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST_F(RelationalNodeReads, QuotedNativeColumnsAndHostileSearchPathRemainSafe) {
    const std::string name = "display.µ\";--";
    sql("ALTER TABLE " + table("person") + " RENAME COLUMN name TO " + quote_identifier(name));
    document["nodes"][0]["properties"]["name"] = name;
    attach("quoted");
    sql("CREATE FUNCTION " + table("deny_name") +
        "(name,name) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT false'; CREATE OPERATOR " +
        quote_identifier(ns) + ".= (LEFTARG=name,RIGHTARG=name,FUNCTION=" + table("deny_name") +
        "); SET search_path=" + quote_identifier(ns) + ",pg_catalog");
    const auto records = read({node("Person", 7)});
    EXPECT_EQ(value(records[0]).properties.at("name")->as<std::string>(), "雪");
}
TEST_F(RelationalNodeReads, UsesNativePkCollationButRejectsChangedIdentitySpelling) {
    sql("CREATE COLLATION " + table("ci") +
        " (provider=icu,locale='und-u-ks-level2',deterministic=false); ALTER TABLE " + table("comment") +
        " ALTER COLUMN id TYPE text COLLATE " + table("ci") + " USING id::text");
    const auto original = node("Comment", "Alice");
    EXPECT_EQ(read({original}).size(), 1);
    sql("UPDATE " + table("comment") + " SET id='ALICE' WHERE id='Alice'");
    EXPECT_THROW((void) read({original}), std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
}
TEST_F(RelationalNodeReads, SelectOnlyRoleReadsWithoutSchemaOrApplicationWrites) {
    const auto role = quote_identifier("reader_" + ns);
    sql("CREATE ROLE " + role + "; GRANT USAGE ON SCHEMA " + quote_identifier(ns) + "," + quote_identifier(other) +
        " TO " + role + "; GRANT SELECT ON ALL TABLES IN SCHEMA " + quote_identifier(ns) + "," +
        quote_identifier(other) + " TO " + role + "; SET ROLE " + role);
    const auto records = read({node("Person", 7), node("Post", uuid)});
    ASSERT_EQ(records.size(), 2);
    EXPECT_EQ(value(records[0]).properties.at("name")->as<std::string>(), "雪");
    sql("RESET ROLE; REVOKE ALL ON ALL TABLES IN SCHEMA " + quote_identifier(ns) + "," + quote_identifier(other) +
        " FROM " + role + "; REVOKE USAGE ON SCHEMA " + quote_identifier(ns) + "," + quote_identifier(other) +
        " FROM " + role + "; DROP ROLE " + role);
}
TEST_F(RelationalNodeReads, ReadsMoreThanOneCursorBatchAndKeepsDuplicateRecordsIndependent) {
    sql("INSERT INTO " + table("person") + "(id) SELECT generate_series(100,2299)");
    attach("large");
    std::vector<domain::SourceIdentity> source;
    for (std::int64_t key = 100; key <= 2299; ++key)
        source.push_back({"app", "Person", key});
    const auto resolved = identities->resolve(source);
    std::vector<NodeRef> input;
    for (auto it = resolved.rbegin(); it != resolved.rend(); ++it)
        input.push_back(it->node);
    input.push_back(input.front());
    auto records = read(input);
    ASSERT_EQ(records.size(), 2201);
    for (std::size_t i = 0; i < records.size(); ++i)
        EXPECT_EQ(value(records[i]).global_id, input[i].global_id);
    std::get<Node>(records.back()->fields[0].data).properties.at("name") =
            std::make_unique<Value>(std::string("changed"));
    EXPECT_TRUE(value(records.front()).properties.at("name")->is_null());
}
TEST_F(RelationalNodeReads, UnsupportedTypeDriftFailsBeforeReturningPartialRecords) {
    sql("ALTER TABLE " + table("person") + " ALTER COLUMN score TYPE numeric USING score::numeric");
    EXPECT_THROW((void) read({node("Post", uuid), node("Person", 7)}), std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
}
TEST_F(RelationalNodeReads, ConstructorRejectsMismatchedCatalogAndInvalidTypedReferences) {
    auto different = document;
    different["nodes"][0]["namespace"] = "other";
    EXPECT_THROW((void) PostgresRelationalNodeReader(parse_relational_mapping(different, schema), *identities),
                 std::invalid_argument);
    auto incorrect = node("Person", 7);
    incorrect.type = post;
    EXPECT_THROW((void) read({incorrect}), std::invalid_argument);
    incorrect.type = unresolved_type_id;
    EXPECT_EQ(value(read({incorrect})[0]).type, person);
}

TEST_F(RelationalNodeReads, OversizedPropertyFailureNamesLogicalAndPhysicalColumn) {
    sql("UPDATE " + table("person") + " SET name=repeat('x',1048577) WHERE id=7");
    try {
        (void) read({node("Person", 7)});
        FAIL() << "accepted oversized property";
    } catch (const std::invalid_argument &error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Person"), std::string::npos);
        EXPECT_NE(message.find("name"), std::string::npos);
        EXPECT_NE(message.find(ns), std::string::npos);
        EXPECT_NE(message.find("1 MiB"), std::string::npos);
    }
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
}
