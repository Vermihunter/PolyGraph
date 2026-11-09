#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>
#include "data_access/adapters/PostgreSQL/RelationalAttachment.hpp"
#include "data_access/adapters/PostgreSQL/RelationalRelationshipTableReader.hpp"

using namespace polygraph;
using namespace polygraph::postgres;
namespace {
    class RelationalRelationships : public ::testing::Test {
    protected:
        std::filesystem::path root;
        std::string ns;
        domain::LogicalGraphSchema schema;
        json document;
        std::unique_ptr<PGconn, decltype(&PQfinish)> db{nullptr, PQfinish};
        std::unique_ptr<RelationalIdentityResolver> identities;
        TypeId person{}, post{}, likes{}, knows{};
        std::string table(const char *name) { return quote_identifier(ns) + "." + quote_identifier(name); }
        void sql(const std::string &s) {
            std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(db.get(), s.c_str()), PQclear);
            ASSERT_TRUE(r);
            ASSERT_TRUE(PQresultStatus(r.get()) == PGRES_COMMAND_OK || PQresultStatus(r.get()) == PGRES_TUPLES_OK)
                    << PQresultErrorMessage(r.get());
        }
        RelationalMapping mapping() { return parse_relational_mapping(document, schema); }
        void SetUp() override {
            auto pattern = (std::filesystem::temp_directory_path() / "polygraph-junction-XXXXXX").string();
            const auto path = mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            root = path;
            ns = "junction_" + root.filename().string().substr(19);
            db.reset(PQconnectdb(std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN")));
            ASSERT_EQ(PQstatus(db.get()), CONNECTION_OK) << PQerrorMessage(db.get());
            sql("CREATE SCHEMA " + quote_identifier(ns) + "; CREATE TABLE " + table("person") +
                "(id bigint PRIMARY KEY); CREATE TABLE " + table("post") + "(id uuid PRIMARY KEY);");
            sql("CREATE TABLE " + table("likes") +
                "(edge_key text PRIMARY KEY,person_id bigint,post_id uuid,weight bigint,active boolean,score "
                "float8,note text,since date,created timestamptz,local_time timestamp,private_data text);");
            sql("CREATE TABLE " + table("knows") + "(src bigint,dst bigint);");
            sql("INSERT INTO " + table("person") + " VALUES(1),(2),(3); INSERT INTO " + table("post") +
                " VALUES('00000000-0000-0000-0000-00000000000a'),('00000000-0000-0000-0000-00000000000b');");
            sql("INSERT INTO " + table("likes") +
                " VALUES('one',1,'00000000-0000-0000-0000-00000000000a',9223372036854775807,true,0.125,'雪','2026-11-"
                "09','2026-11-09 10:00:00+02','2026-11-09 "
                "10:00:00','secret'),('two',1,'00000000-0000-0000-0000-00000000000a',NULL,false,NULL,NULL,NULL,NULL,"
                "NULL,NULL),('three',2,'00000000-0000-0000-0000-00000000000b',3,true,1,'x','2020-01-01','2020-01-01+00'"
                ",'2020-01-01',NULL);");
            sql("INSERT INTO " + table("knows") + " VALUES(1,1),(1,1),(1,2),(2,1);");
            person = schema.add_node_type("Person");
            post = schema.add_node_type("Post");
            likes = schema.add_edge_type("Likes", person, post, true,
                                         {{"weight", domain::PropertyType::integer},
                                          {"active", domain::PropertyType::boolean},
                                          {"score", domain::PropertyType::_float},
                                          {"note", domain::PropertyType::string},
                                          {"since", domain::PropertyType::date},
                                          {"created", domain::PropertyType::datetime},
                                          {"local", domain::PropertyType::datetime}});
            knows = schema.add_edge_type("Knows", person, person);
            document = {{"format", "polygraph.postgresql-relational"},
                        {"version", 1},
                        {"nodes", json::array({{{"type", "Person"},
                                                {"schema", ns},
                                                {"table", "person"},
                                                {"primaryKey", "id"},
                                                {"namespace", "app"},
                                                {"keyCodec", "int64"}},
                                               {{"type", "Post"},
                                                {"schema", ns},
                                                {"table", "post"},
                                                {"primaryKey", "id"},
                                                {"namespace", "app"},
                                                {"keyCodec", "uuid"}}})},
                        {"relationships", json::array({{{"type", "Likes"},
                                                        {"kind", "table"},
                                                        {"schema", ns},
                                                        {"table", "likes"},
                                                        {"fromColumn", "person_id"},
                                                        {"toColumn", "post_id"},
                                                        {"primaryKey", "edge_key"},
                                                        {"keyCodec", "string"},
                                                        {"properties",
                                                         {{"weight", "weight"},
                                                          {"active", "active"},
                                                          {"score", "score"},
                                                          {"note", "note"},
                                                          {"since", "since"},
                                                          {"created", "created"},
                                                          {"local", "local_time"}}}},
                                                       {{"type", "Knows"},
                                                        {"kind", "table"},
                                                        {"schema", ns},
                                                        {"table", "knows"},
                                                        {"fromColumn", "src"},
                                                        {"toColumn", "dst"}}})}};
            const auto manifest = attach_relational_nodes(db.get(), mapping(), {.output_dir = root / "catalog"});
            identities = std::make_unique<RelationalIdentityResolver>(manifest, mapping());
        }
        void TearDown() override {
            if (db && PQstatus(db.get()) == CONNECTION_OK) {
                if (PQtransactionStatus(db.get()) != PQTRANS_IDLE)
                    sql("ROLLBACK");
                sql("RESET ROLE; DROP SCHEMA IF EXISTS " + quote_identifier(ns) + " CASCADE; DROP ROLE IF EXISTS " +
                    quote_identifier("reader_" + ns));
            }
            if (!root.empty())
                std::filesystem::remove_all(root);
        }
        NodeRef node(const char *type, json key) {
            const std::array<domain::SourceIdentity, 1> source{{{"app", type, std::move(key)}}};
            return identities->resolve(source)[0].node;
        }
        void text_keys_with_collations(bool insensitive_primary_key) {
            sql("CREATE COLLATION " + table("ci") + " (provider=icu,locale='und-u-ks-level2',deterministic=false)");
            const auto pk_collation = insensitive_primary_key ? table("ci") : "pg_catalog.\"C\"";
            const auto edge_collation = insensitive_primary_key ? "pg_catalog.\"C\"" : table("ci");
            sql("ALTER TABLE " + table("person") + " ALTER COLUMN id TYPE text COLLATE " + pk_collation +
                " USING id::text; TRUNCATE " + table("person") + "," + table("knows"));
            sql("ALTER TABLE " + table("knows") + " ALTER COLUMN src TYPE text COLLATE " + edge_collation +
                " USING src::text, ALTER COLUMN dst TYPE text COLLATE " + edge_collation + " USING dst::text");
            sql("INSERT INTO " + table("person") + " VALUES('Alice'),('" +
                (insensitive_primary_key ? std::string("Carol") : std::string("alice")) + "'),('Bob')");
            sql("INSERT INTO " + table("knows") + " VALUES('" +
                (insensitive_primary_key ? std::string("alice") : std::string("Alice")) + "','Bob')");
            document["nodes"][0]["keyCodec"] = "string";
            document["relationships"].erase(0);
            const auto manifest = attach_relational_nodes(db.get(), mapping(), {.output_dir = root / "collated"});
            identities = std::make_unique<RelationalIdentityResolver>(manifest, mapping());
        }

        std::vector<RelationalRelationshipRow>
        read(std::vector<NodeRef> frontier, RelationshipLookupDirection direction, std::vector<TypeId> types = {}) {
            RelationalRelationshipTableReader reader(mapping(), *identities);
            std::vector<RelationalRelationshipRow> result;
            reader.stream(db.get(), frontier,
                          {.relationship_types = std::move(types), .direction = direction, .batch_size = 1},
                          [&](auto rows) { result.insert(result.end(), rows.begin(), rows.end()); });
            return result;
        }
    };
} // namespace
TEST_F(RelationalRelationships, OutgoingResolvesBothEndpointsAndNativeAttributes) {
    const auto origin = node("Person", 1);
    const auto rows = read({origin}, RelationshipLookupDirection::outgoing, {likes});
    ASSERT_EQ(rows.size(), 2);
    const auto it = std::ranges::find_if(rows, [](const auto &r) { return r.external_key == json("one"); });
    ASSERT_NE(it, rows.end());
    EXPECT_EQ(it->origin.global_id, origin.global_id);
    EXPECT_EQ(it->from.node.global_id, origin.global_id);
    EXPECT_EQ(it->to.node.global_id, node("Post", "00000000-0000-0000-0000-00000000000a").global_id);
    EXPECT_EQ(it->properties["weight"], INT64_MAX);
    EXPECT_TRUE(it->properties["active"].get<bool>());
    EXPECT_EQ(it->properties["score"], 0.125);
    EXPECT_EQ(it->properties["note"], "雪");
    EXPECT_EQ(it->properties["since"], "2026-11-09");
    EXPECT_EQ(it->properties["created"], "2026-11-09T08:00:00Z");
    EXPECT_EQ(it->properties["local"], "2026-11-09T10:00:00");
    const auto other = std::ranges::find_if(rows, [](const auto &r) { return r.external_key == json("two"); });
    ASSERT_NE(other, rows.end());
    EXPECT_TRUE(other->properties["weight"].is_null());
    EXPECT_FALSE(other->properties["active"].get<bool>());
}
TEST_F(RelationalRelationships, IncomingKeepsStoredOrientationAndMarksReverseMatch) {
    const auto target = node("Post", "00000000-0000-0000-0000-00000000000a");
    const auto rows = read({target}, RelationshipLookupDirection::incoming, {likes});
    ASSERT_EQ(rows.size(), 2);
    EXPECT_TRUE(rows[0].reverse);
    EXPECT_EQ(rows[0].origin.global_id, target.global_id);
    EXPECT_EQ(rows[0].to.node.global_id, target.global_id);
    EXPECT_EQ(rows[0].from.node.global_id, node("Person", 1).global_id);
    EXPECT_TRUE(read({target}, RelationshipLookupDirection::outgoing, {likes}).empty());
}
TEST_F(RelationalRelationships, BothDirectionsPreserveParallelRowsAndEmitEachSelfLoopOnce) {
    const auto p = node("Person", 1);
    const auto rows = read({p, p}, RelationshipLookupDirection::both, {knows});
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(std::ranges::count_if(rows, [](const auto &r) { return r.from.node.global_id == r.to.node.global_id; }),
              2);
    for (const auto &r: rows)
        EXPECT_FALSE(r.external_key.has_value());
}
TEST_F(RelationalRelationships, MatchingNullDanglingAndUncatalogedEndpointsFailExplicitly) {
    sql("INSERT INTO " + table("knows") + " VALUES(3,NULL)");
    EXPECT_THROW((void) read({node("Person", 3)}, RelationshipLookupDirection::outgoing, {knows}),
                 std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
    sql("DELETE FROM " + table("knows") + " WHERE src=3; INSERT INTO " + table("knows") + " VALUES(3,999)");
    EXPECT_THROW((void) read({node("Person", 3)}, RelationshipLookupDirection::outgoing, {knows}),
                 std::invalid_argument);
    sql("INSERT INTO " + table("person") + " VALUES(999)");
    EXPECT_THROW((void) read({node("Person", 3)}, RelationshipLookupDirection::outgoing, {knows}),
                 std::invalid_argument);
}
TEST_F(RelationalRelationships, RestoresSessionSettingsAndRollsBackCallbackFailures) {
    sql("SET client_encoding='LATIN1'; SET DateStyle='SQL,DMY'; SET TimeZone='Europe/Bratislava'");
    EXPECT_EQ(read({node("Person", 1)}, RelationshipLookupDirection::outgoing, {likes}).size(), 2);
    EXPECT_EQ(std::string(PQparameterStatus(db.get(), "client_encoding")), "LATIN1");
    EXPECT_EQ(std::string(PQparameterStatus(db.get(), "TimeZone")), "Europe/Bratislava");
    RelationalRelationshipTableReader reader(mapping(), *identities);
    const std::array<NodeRef, 1> start{node("Person", 1)};
    EXPECT_THROW(reader.stream(db.get(), start, {}, [](auto) { throw std::runtime_error("consumer failed"); }),
                 std::runtime_error);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
}
TEST_F(RelationalRelationships, EmptyFrontierAndUnknownTypesAreHandledDeliberately) {
    EXPECT_TRUE(read({}, RelationshipLookupDirection::both).empty());
    EXPECT_THROW((void) read({node("Person", 1)}, RelationshipLookupDirection::outgoing, {9999}),
                 std::invalid_argument);
    sql("BEGIN");
    EXPECT_THROW((void) read({node("Person", 1)}, RelationshipLookupDirection::both), std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_INTRANS);
    sql("ROLLBACK");
}
int main(int argc, char **argv) {
    if (!std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN"))
        return 77;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST_F(RelationalRelationships, BothModeCannotHideNullIncomingEndpoints) {
    sql("INSERT INTO " + table("knows") + " VALUES(NULL,3)");
    EXPECT_THROW((void) read({node("Person", 3)}, RelationshipLookupDirection::both, {knows}), std::invalid_argument);
}
TEST_F(RelationalRelationships, FiltersTargetsAndStreamsBoundedRowsAcrossLargeFrontier) {
    sql("INSERT INTO " + table("knows") + " SELECT 3,1 FROM generate_series(1,2100)");
    RelationalRelationshipTableReader reader(mapping(), *identities);
    const std::array<NodeRef, 2> start{node("Person", 3), node("Post", "00000000-0000-0000-0000-00000000000b")};
    std::size_t count = 0;
    reader.stream(db.get(), start,
                  {.relationship_types = {knows},
                   .target_types = {person},
                   .direction = RelationshipLookupDirection::outgoing,
                   .batch_size = 17},
                  [&](auto batch) {
                      EXPECT_LE(batch.size(), 17);
                      count += batch.size();
                  });
    EXPECT_EQ(count, 2100);
    count = 0;
    reader.stream(db.get(), start, {.relationship_types = {knows}, .target_types = {post}},
                  [&](auto rows) { count += rows.size(); });
    EXPECT_EQ(count, 0);
}
TEST_F(RelationalRelationships, RejectsNonFiniteNumbersAndStaleSourceRows) {
    sql("UPDATE " + table("likes") + " SET score='NaN' WHERE edge_key='one'");
    EXPECT_THROW((void) read({node("Person", 1)}, RelationshipLookupDirection::outgoing, {likes}),
                 std::invalid_argument);
    sql("INSERT INTO " + table("knows") + " VALUES(3,1); DELETE FROM " + table("person") + " WHERE id=3");
    EXPECT_THROW((void) read({node("Person", 3)}, RelationshipLookupDirection::outgoing, {knows}),
                 std::invalid_argument);
}
TEST_F(RelationalRelationships, QuotedPhysicalNamesAndHostileSearchPathAreSafe) {
    const std::string name = "likes.µ\";--";
    sql("ALTER TABLE " + table("likes") + " RENAME TO " + quote_identifier(name));
    document["relationships"][0]["table"] = name;
    const auto manifest = attach_relational_nodes(db.get(), mapping(), {.output_dir = root / "quoted"});
    identities = std::make_unique<RelationalIdentityResolver>(manifest, mapping());
    sql("CREATE FUNCTION " + table("deny_name") +
        "(name,name) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT false'; CREATE OPERATOR " +
        quote_identifier(ns) + ".= (LEFTARG=name,RIGHTARG=name,FUNCTION=" + table("deny_name") +
        "); SET search_path=" + quote_identifier(ns) + ",pg_catalog");
    EXPECT_EQ(read({node("Person", 1)}, RelationshipLookupDirection::outgoing, {likes}).size(), 2);
}
TEST_F(RelationalRelationships, SnapshotRequiresTableSelectButNoApplicationWritePrivileges) {
    const auto role = quote_identifier("reader_" + ns);
    sql("CREATE ROLE " + role + "; GRANT USAGE ON SCHEMA " + quote_identifier(ns) + " TO " + role +
        "; GRANT SELECT(id) ON " + table("person") + "," + table("post") + " TO " + role +
        "; GRANT SELECT(src,dst) ON " + table("knows") + " TO " + role +
        "; GRANT SELECT(edge_key,person_id,post_id,weight,active,score,note,since,created,local_time) ON " +
        table("likes") + " TO " + role + "; SET ROLE " + role);
    EXPECT_THROW((void) read({node("Person", 1)}, RelationshipLookupDirection::outgoing, {likes}),
                 std::invalid_argument);
    sql("RESET ROLE; GRANT SELECT ON ALL TABLES IN SCHEMA " + quote_identifier(ns) + " TO " + role + "; SET ROLE " +
        role);
    EXPECT_EQ(read({node("Person", 1)}, RelationshipLookupDirection::outgoing, {likes}).size(), 2);
    sql("RESET ROLE; REVOKE ALL ON ALL TABLES IN SCHEMA " + quote_identifier(ns) + " FROM " + role +
        "; REVOKE SELECT(id) ON " + table("person") + "," + table("post") + " FROM " + role +
        "; REVOKE SELECT(src,dst) ON " + table("knows") + " FROM " + role +
        "; REVOKE SELECT(edge_key,person_id,post_id,weight,active,score,note,since,created,local_time) ON " +
        table("likes") + " FROM " + role + "; REVOKE USAGE ON SCHEMA " + quote_identifier(ns) + " FROM " + role +
        "; DROP ROLE " + role);
}

TEST_F(RelationalRelationships, FloatPropertiesDoNotDependOnCallerOutputPrecision) {
    sql("UPDATE " + table("likes") + " SET score=1.2345678901234567 WHERE edge_key='one'; SET extra_float_digits=-15");
    const auto rows = read({node("Person", 1)}, RelationshipLookupDirection::outgoing, {likes});
    const auto found = std::ranges::find_if(rows, [](const auto &r) { return r.external_key == json("one"); });
    ASSERT_NE(found, rows.end());
    EXPECT_DOUBLE_EQ(found->properties["score"].get<double>(), 1.2345678901234567);
    std::unique_ptr<PGresult, decltype(&PQclear)> setting(PQexec(db.get(), "SHOW extra_float_digits"), PQclear);
    ASSERT_EQ(PQresultStatus(setting.get()), PGRES_TUPLES_OK);
    EXPECT_EQ(std::string(PQgetvalue(setting.get(), 0, 0)), "-15");
}

TEST_F(RelationalRelationships, PrimaryKeyCollationControlsReferenceIdentity) {
    text_keys_with_collations(false);
    const auto alice = node("Person", "Alice"), other = node("Person", "alice");
    const auto rows = read({alice, other}, RelationshipLookupDirection::outgoing, {knows});
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].origin.global_id, alice.global_id);
    EXPECT_EQ(rows[0].from.node.global_id, alice.global_id);
}
TEST_F(RelationalRelationships, CaseInsensitivePrimaryKeysResolveCanonicalStoredKeys) {
    text_keys_with_collations(true);
    const auto alice = node("Person", "Alice");
    const auto rows = read({alice}, RelationshipLookupDirection::outgoing, {knows});
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].from.source.value, "Alice");
    EXPECT_EQ(rows[0].to.source.value, "Bob");
}
TEST_F(RelationalRelationships, DeletedIsolatedFrontierDoesNotLookLikeAnEmptyAdjacency) {
    const auto isolated = node("Person", 3);
    sql("DELETE FROM " + table("person") + " WHERE id=3");
    EXPECT_THROW((void) read({isolated}, RelationshipLookupDirection::outgoing, {knows}), std::invalid_argument);
}

TEST_F(RelationalRelationships, MultipleOriginsAndIncomingSelfLoopsPreserveMultiplicity) {
    const auto one = node("Person", 1), two = node("Person", 2);
    const auto incoming = read({one}, RelationshipLookupDirection::incoming, {knows});
    EXPECT_EQ(incoming.size(), 3);
    const auto both = read({one, two}, RelationshipLookupDirection::both, {knows});
    EXPECT_EQ(both.size(), 6);
    EXPECT_EQ(std::ranges::count_if(both, [&](const auto &r) { return r.origin.global_id == two.global_id; }), 2);
    for (const auto &row: both)
        EXPECT_EQ(row.origin.global_id, row.reverse ? row.to.node.global_id : row.from.node.global_id);
}
