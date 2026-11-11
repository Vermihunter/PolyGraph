#include <algorithm>
#include <cstdlib>
#include <gtest/gtest.h>
#include <unistd.h>
#include "data_access/adapters/PostgreSQL/RelationalAttachment.hpp"
#include "data_access/adapters/PostgreSQL/RelationalForeignKeyReader.hpp"
#include "data_access/adapters/PostgreSQL/RelationalRelationshipTableReader.hpp"

using namespace polygraph;
using namespace polygraph::postgres;
namespace {
    class RelationalForeignKeys : public ::testing::Test {
    protected:
        std::filesystem::path root;
        std::string ns;
        domain::LogicalGraphSchema schema;
        json document;
        std::unique_ptr<PGconn, decltype(&PQfinish)> db{nullptr, PQfinish};
        std::unique_ptr<RelationalIdentityResolver> identities;
        TypeId person{}, post{}, comment{}, authors{}, reply{}, reviewed{}, managed{}, tagged{};
        const std::string a = "00000000-0000-0000-0000-00000000000a", b = "00000000-0000-0000-0000-00000000000b",
                          c = "00000000-0000-0000-0000-00000000000c";
        std::string table(const char *name) { return quote_identifier(ns) + "." + quote_identifier(name); }
        void sql(const std::string &text) {
            std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(db.get(), text.c_str()), PQclear);
            ASSERT_TRUE(r);
            ASSERT_TRUE(PQresultStatus(r.get()) == PGRES_COMMAND_OK || PQresultStatus(r.get()) == PGRES_TUPLES_OK)
                    << PQresultErrorMessage(r.get());
        }
        RelationalMapping mapping() { return parse_relational_mapping(document, schema); }
        void SetUp() override {
            auto pattern = (std::filesystem::temp_directory_path() / "polygraph-fk-XXXXXX").string();
            const auto path = mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            root = path;
            ns = "fk_" + root.filename().string().substr(13);
            db.reset(PQconnectdb(std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN")));
            ASSERT_EQ(PQstatus(db.get()), CONNECTION_OK) << PQerrorMessage(db.get());
            sql("CREATE SCHEMA " + quote_identifier(ns) + "; CREATE TABLE " + table("person") +
                "(id bigint PRIMARY KEY,manager_id bigint REFERENCES " + table("person") + "(id)); CREATE TABLE " +
                table("post") + "(id uuid PRIMARY KEY,author_id bigint REFERENCES " + table("person") +
                "(id)); CREATE TABLE " + table("comment") + "(id text PRIMARY KEY,reply_to_post_id uuid REFERENCES " +
                table("post") + "(id),reviewer_id bigint REFERENCES " + table("person") + "(id)); CREATE TABLE " +
                table("tagged") + "(person_id bigint,post_id uuid)");
            sql("INSERT INTO " + table("person") + " VALUES(1,1),(2,1),(3,NULL); INSERT INTO " + table("post") +
                " VALUES('" + a + "',1),('" + b + "',2),('" + c + "',NULL); INSERT INTO " + table("comment") +
                " VALUES('c1','" + a + "',2),('c2','" + a + "',NULL),('c3',NULL,1); INSERT INTO " + table("tagged") +
                " VALUES(1,'" + a + "')");
            person = schema.add_node_type("Person");
            post = schema.add_node_type("Post");
            comment = schema.add_node_type("Comment");
            authors = schema.add_edge_type("Authors", person, post);
            reply = schema.add_edge_type("ReplyOf", comment, post);
            reviewed = schema.add_edge_type("ReviewedBy", comment, person);
            managed = schema.add_edge_type("ManagedBy", person, person);
            tagged = schema.add_edge_type("Tagged", person, post);
            document = {
                    {"format", "polygraph.postgresql-relational"},
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
                                            {"keyCodec", "uuid"}},
                                           {{"type", "Comment"},
                                            {"schema", ns},
                                            {"table", "comment"},
                                            {"primaryKey", "id"},
                                            {"namespace", "app"},
                                            {"keyCodec", "string"}}})},
                    {"relationships",
                     json::array(
                             {{{"type", "Authors"}, {"kind", "foreignKey"}, {"owner", "to"}, {"column", "author_id"}},
                              {{"type", "ReplyOf"},
                               {"kind", "foreignKey"},
                               {"owner", "from"},
                               {"column", "reply_to_post_id"}},
                              {{"type", "ReviewedBy"},
                               {"kind", "foreignKey"},
                               {"owner", "from"},
                               {"column", "reviewer_id"}},
                              {{"type", "ManagedBy"},
                               {"kind", "foreignKey"},
                               {"owner", "from"},
                               {"column", "manager_id"}},
                              {{"type", "Tagged"},
                               {"kind", "table"},
                               {"schema", ns},
                               {"table", "tagged"},
                               {"fromColumn", "person_id"},
                               {"toColumn", "post_id"}}})}};
            attach("initial");
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
        void attach(const char *generation) {
            const auto manifest = attach_relational_nodes(db.get(), mapping(), {.output_dir = root / generation});
            identities = std::make_unique<RelationalIdentityResolver>(manifest, mapping());
        }
        NodeRef node(const char *type, json key) {
            const std::array<domain::SourceIdentity, 1> source{{{"app", type, std::move(key)}}};
            return identities->resolve(source)[0].node;
        }
        std::vector<RelationalRelationshipRow>
        read(std::vector<NodeRef> frontier, RelationshipLookupDirection direction, std::vector<TypeId> types = {}) {
            RelationalForeignKeyReader reader(mapping(), *identities);
            std::vector<RelationalRelationshipRow> result;
            reader.stream(db.get(), frontier,
                          {.relationship_types = std::move(types), .direction = direction, .batch_size = 1},
                          [&](auto rows) { result.insert(result.end(), rows.begin(), rows.end()); });
            return result;
        }
    };
} // namespace
TEST_F(RelationalForeignKeys, OwnerFromResolvesForwardAndReverseUuidReferences) {
    const auto source = node("Comment", "c1"), target = node("Post", a);
    const auto forward = read({source}, RelationshipLookupDirection::outgoing, {reply});
    ASSERT_EQ(forward.size(), 1);
    EXPECT_EQ(forward[0].from.node.global_id, source.global_id);
    EXPECT_EQ(forward[0].to.node.global_id, target.global_id);
    EXPECT_FALSE(forward[0].reverse);
    EXPECT_FALSE(forward[0].external_key);
    EXPECT_TRUE(forward[0].properties.empty());
    const auto backward = read({target}, RelationshipLookupDirection::incoming, {reply});
    ASSERT_EQ(backward.size(), 2);
    for (const auto &r: backward) {
        EXPECT_TRUE(r.reverse);
        EXPECT_EQ(r.origin.global_id, target.global_id);
        EXPECT_EQ(r.to.node.global_id, target.global_id);
    }
}
TEST_F(RelationalForeignKeys, OwnerToDoesNotReverseLogicalGraphDirection) {
    const auto source = node("Person", 1), target = node("Post", a);
    const auto forward = read({source}, RelationshipLookupDirection::outgoing, {authors});
    ASSERT_EQ(forward.size(), 1);
    EXPECT_EQ(forward[0].to.node.global_id, target.global_id);
    const auto backward = read({target}, RelationshipLookupDirection::incoming, {authors});
    ASSERT_EQ(backward.size(), 1);
    EXPECT_EQ(backward[0].from.node.global_id, source.global_id);
    EXPECT_TRUE(read({source}, RelationshipLookupDirection::incoming, {authors}).empty());
}
TEST_F(RelationalForeignKeys, NullMeansNoEdgeForBothOwnerOrientations) {
    const auto comment = node("Comment", "c3"), post = node("Post", c);
    EXPECT_TRUE(read({comment}, RelationshipLookupDirection::outgoing, {reply}).empty());
    EXPECT_TRUE(read({comment}, RelationshipLookupDirection::both, {reply}).empty());
    EXPECT_TRUE(read({post}, RelationshipLookupDirection::incoming, {authors}).empty());
    EXPECT_TRUE(read({post}, RelationshipLookupDirection::both, {authors}).empty());
}
TEST_F(RelationalForeignKeys, SelfLoopsAndMultipleOriginsHaveStableMultiplicity) {
    const auto one = node("Person", 1), two = node("Person", 2);
    EXPECT_EQ(read({one}, RelationshipLookupDirection::outgoing, {managed}).size(), 1);
    EXPECT_EQ(read({one}, RelationshipLookupDirection::incoming, {managed}).size(), 2);
    const auto both = read({one, one, two}, RelationshipLookupDirection::both, {managed});
    ASSERT_EQ(both.size(), 3);
    EXPECT_EQ(std::ranges::count_if(both, [](const auto &r) { return r.from.node.global_id == r.to.node.global_id; }),
              1);
    for (const auto &r: both)
        EXPECT_EQ(r.origin.global_id, r.reverse ? r.to.node.global_id : r.from.node.global_id);
}
TEST_F(RelationalForeignKeys, MixedMappingReadersRetainTheirOwnRelationshipScope) {
    const auto source = node("Person", 1);
    const auto fk = read({source}, RelationshipLookupDirection::both);
    EXPECT_TRUE(std::ranges::none_of(fk, [&](const auto &r) { return r.type == tagged; }));
    EXPECT_THROW((void) read({source}, RelationshipLookupDirection::outgoing, {tagged}), std::invalid_argument);
    RelationalRelationshipTableReader tables(mapping(), *identities);
    const std::array<NodeRef, 1> start{source};
    std::size_t count = 0;
    tables.stream(db.get(), start, {}, [&](auto rows) {
        for (const auto &r: rows)
            EXPECT_EQ(r.type, tagged);
        count += rows.size();
    });
    EXPECT_EQ(count, 1);
}
TEST_F(RelationalForeignKeys, NewUncataloguedReferenceFailsAndConnectionIsReusable) {
    sql("INSERT INTO " + table("post") + " VALUES('00000000-0000-0000-0000-00000000000d',NULL); UPDATE " +
        table("comment") + " SET reply_to_post_id='00000000-0000-0000-0000-00000000000d' WHERE id='c1'");
    EXPECT_THROW((void) read({node("Comment", "c1")}, RelationshipLookupDirection::outgoing, {reply}),
                 std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
    EXPECT_EQ(read({node("Comment", "c2")}, RelationshipLookupDirection::outgoing, {reply}).size(), 1);
}
TEST_F(RelationalForeignKeys, NonNullBrokenReferenceFailsIfConstraintDataWereBypassed) {
    sql("SET session_replication_role=replica; UPDATE " + table("comment") +
        " SET reviewer_id=999 WHERE id='c1'; SET session_replication_role=origin");
    EXPECT_THROW((void) read({node("Comment", "c1")}, RelationshipLookupDirection::outgoing, {reviewed}),
                 std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
}
TEST_F(RelationalForeignKeys, ConsumerFailureRestoresSettingsAndLeavesApplicationRowsUnchanged) {
    sql("SET client_encoding='LATIN1'; SET DateStyle='SQL,DMY'; SET TimeZone='Europe/Bratislava'");
    RelationalForeignKeyReader reader(mapping(), *identities);
    const std::array<NodeRef, 1> start{node("Post", a)};
    EXPECT_THROW(reader.stream(db.get(), start, {.relationship_types = {reply}},
                               [](auto) { throw std::runtime_error("consumer"); }),
                 std::runtime_error);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
    EXPECT_EQ(std::string(PQparameterStatus(db.get(), "client_encoding")), "LATIN1");
    EXPECT_EQ(read({node("Post", a)}, RelationshipLookupDirection::incoming, {reply}).size(), 2);
}
int main(int argc, char **argv) {
    if (!std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN"))
        return 77;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST_F(RelationalForeignKeys, FiltersAndMultiTypeOriginsPreserveLogicalDirection) {
    RelationalForeignKeyReader reader(mapping(), *identities);
    const auto p = node("Person", 1), target = node("Post", a);
    const std::array<NodeRef, 2> start{p, target};
    std::vector<RelationalRelationshipRow> result;
    reader.stream(db.get(), start, {.relationship_types = {authors}},
                  [&](auto rows) { result.insert(result.end(), rows.begin(), rows.end()); });
    ASSERT_EQ(result.size(), 2);
    for (const auto &r: result) {
        EXPECT_EQ(r.from.node.global_id, p.global_id);
        EXPECT_EQ(r.to.node.global_id, target.global_id);
        EXPECT_EQ(r.origin.global_id, r.reverse ? target.global_id : p.global_id);
    }
    const std::array<NodeRef, 1> from{target};
    std::size_t count = 0;
    reader.stream(db.get(), from,
                  {.relationship_types = {reply},
                   .target_types = {person},
                   .direction = RelationshipLookupDirection::incoming},
                  [&](auto rows) { count += rows.size(); });
    EXPECT_EQ(count, 0);
    reader.stream(db.get(), from,
                  {.relationship_types = {reply},
                   .target_types = {comment},
                   .direction = RelationshipLookupDirection::incoming},
                  [&](auto rows) { count += rows.size(); });
    EXPECT_EQ(count, 2);
}
TEST_F(RelationalForeignKeys, LargeReverseFanoutUsesBoundedBatches) {
    sql("INSERT INTO " + table("person") + " SELECT generate_series(100,2399),1");
    attach("large");
    RelationalForeignKeyReader reader(mapping(), *identities);
    const std::array<NodeRef, 1> start{node("Person", 1)};
    std::size_t count = 0;
    reader.stream(
            db.get(), start,
            {.relationship_types = {managed}, .direction = RelationshipLookupDirection::incoming, .batch_size = 31},
            [&](auto rows) {
                EXPECT_LE(rows.size(), 31);
                count += rows.size();
            });
    EXPECT_EQ(count, 2302);
}
TEST_F(RelationalForeignKeys, QuotedForeignKeyAndHostileSearchPathAreSafe) {
    const std::string column = "reply.µ\";--";
    sql("ALTER TABLE " + table("comment") + " RENAME COLUMN reply_to_post_id TO " + quote_identifier(column));
    document["relationships"][1]["column"] = column;
    attach("quoted");
    sql("CREATE FUNCTION " + table("deny_uuid") +
        "(uuid,uuid) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT false'; CREATE OPERATOR " +
        quote_identifier(ns) + ".= (LEFTARG=uuid,RIGHTARG=uuid,FUNCTION=" + table("deny_uuid") +
        "); SET search_path=" + quote_identifier(ns) + ",pg_catalog; SET client_encoding='LATIN1'");
    EXPECT_EQ(read({node("Comment", "c1")}, RelationshipLookupDirection::outgoing, {reply}).size(), 1);
    EXPECT_EQ(std::string(PQparameterStatus(db.get(), "client_encoding")), "LATIN1");
}
TEST_F(RelationalForeignKeys, RequiresDeclaredConstraintAndPreservesCallerTransactions) {
    sql("BEGIN");
    EXPECT_THROW((void) read({node("Comment", "c1")}, RelationshipLookupDirection::outgoing, {reply}),
                 std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_INTRANS);
    sql("ROLLBACK");
    sql("ALTER TABLE " + table("comment") + " DROP CONSTRAINT comment_reply_to_post_id_fkey");
    EXPECT_THROW((void) read({node("Comment", "c1")}, RelationshipLookupDirection::outgoing, {reply}),
                 std::invalid_argument);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
}
TEST_F(RelationalForeignKeys, ReaderRoleCannotWriteOrAlterApplicationSchema) {
    const auto role = quote_identifier("reader_" + ns);
    sql("CREATE ROLE " + role + "; GRANT USAGE ON SCHEMA " + quote_identifier(ns) + " TO " + role +
        "; GRANT SELECT ON ALL TABLES IN SCHEMA " + quote_identifier(ns) + " TO " + role + "; SET ROLE " + role);
    EXPECT_EQ(read({node("Post", a)}, RelationshipLookupDirection::incoming, {reply}).size(), 2);
    sql("RESET ROLE");
    // Fixture counts and FK constraints remain intact after the SELECT-only call.
    std::unique_ptr<PGresult, decltype(&PQclear)> check(
            PQexec(db.get(),
                   ("SELECT count(*) FROM " + table("comment") + " WHERE reply_to_post_id='" + a + "'").c_str()),
            PQclear);
    ASSERT_EQ(PQresultStatus(check.get()), PGRES_TUPLES_OK);
    EXPECT_EQ(std::string(PQgetvalue(check.get(), 0, 0)), "2");
    sql("REVOKE ALL ON ALL TABLES IN SCHEMA " + quote_identifier(ns) + " FROM " + role + "; REVOKE USAGE ON SCHEMA " +
        quote_identifier(ns) + " FROM " + role + "; DROP ROLE " + role);
}
TEST_F(RelationalForeignKeys, CaseInsensitiveReferencedPkResolvesCanonicalKeyWithOwnerFrom) {
    sql("CREATE COLLATION " + table("ci") +
        " (provider=icu,locale='und-u-ks-level2',deterministic=false); CREATE TABLE " + table("text_parent") +
        "(id text COLLATE " + table("ci") + " PRIMARY KEY); CREATE TABLE " + table("text_child") +
        "(id text PRIMARY KEY,parent_id text COLLATE " + table("ci") + " REFERENCES " + table("text_parent") +
        "(id)); INSERT INTO " + table("text_parent") + " VALUES('Alice'); INSERT INTO " + table("text_child") +
        " VALUES('x','alice'),('null',NULL)");
    const auto parent = schema.add_node_type("TextParent"), child = schema.add_node_type("TextChild");
    const auto edge = schema.add_edge_type("TextLink", child, parent);
    document["nodes"] = json::array({{{"type", "TextParent"},
                                      {"schema", ns},
                                      {"table", "text_parent"},
                                      {"primaryKey", "id"},
                                      {"namespace", "app"},
                                      {"keyCodec", "string"}},
                                     {{"type", "TextChild"},
                                      {"schema", ns},
                                      {"table", "text_child"},
                                      {"primaryKey", "id"},
                                      {"namespace", "app"},
                                      {"keyCodec", "string"}}});
    document["relationships"] =
            json::array({{{"type", "TextLink"}, {"kind", "foreignKey"}, {"owner", "from"}, {"column", "parent_id"}}});
    attach("ci");
    const auto rows = read({node("TextChild", "x")}, RelationshipLookupDirection::outgoing, {edge});
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].to.source.value, "Alice");
    EXPECT_EQ(read({node("TextParent", "Alice")}, RelationshipLookupDirection::incoming, {edge}).size(), 1);
    EXPECT_TRUE(read({node("TextChild", "null")}, RelationshipLookupDirection::both, {edge}).empty());
}
TEST_F(RelationalForeignKeys, ReferencedPkCollationOverridesForeignColumnWithOwnerTo) {
    sql("CREATE COLLATION " + table("ci") +
        " (provider=icu,locale='und-u-ks-level2',deterministic=false); CREATE TABLE " + table("text_parent") +
        "(id text COLLATE pg_catalog.\"C\" PRIMARY KEY); CREATE TABLE " + table("text_child") +
        "(id text PRIMARY KEY,parent_id text COLLATE pg_catalog.\"POSIX\"" + " REFERENCES " + table("text_parent") +
        "(id)); INSERT INTO " + table("text_parent") + " VALUES('Alice'),('alice'); INSERT INTO " +
        table("text_child") + " VALUES('x','Alice')");
    const auto parent = schema.add_node_type("TextParent"), child = schema.add_node_type("TextChild");
    const auto edge = schema.add_edge_type("TextLink", parent, child);
    document["nodes"] = json::array({{{"type", "TextParent"},
                                      {"schema", ns},
                                      {"table", "text_parent"},
                                      {"primaryKey", "id"},
                                      {"namespace", "app"},
                                      {"keyCodec", "string"}},
                                     {{"type", "TextChild"},
                                      {"schema", ns},
                                      {"table", "text_child"},
                                      {"primaryKey", "id"},
                                      {"namespace", "app"},
                                      {"keyCodec", "string"}}});
    document["relationships"] =
            json::array({{{"type", "TextLink"}, {"kind", "foreignKey"}, {"owner", "to"}, {"column", "parent_id"}}});
    attach("ci");
    const auto source = node("TextParent", "Alice"), other = node("TextParent", "alice");
    const auto rows = read({source, other}, RelationshipLookupDirection::outgoing, {edge});
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].origin.global_id, source.global_id);
    EXPECT_EQ(rows[0].to.source.value, "x");
}

TEST_F(RelationalForeignKeys, FkOnlyMappingRequiresNoJunctionOrGenericEdgeTables) {
    document["relationships"].erase(4);
    sql("DROP TABLE " + table("tagged"));
    attach("only-fk");
    EXPECT_EQ(read({node("Post", a)}, RelationshipLookupDirection::incoming, {reply}).size(), 2);
    const auto query = "SELECT count(*) FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON "
                       "n.oid=c.relnamespace WHERE n.nspname='" +
                       ns + "' AND c.relkind='r'";
    std::unique_ptr<PGresult, decltype(&PQclear)> relations(PQexec(db.get(), query.c_str()), PQclear);
    ASSERT_EQ(PQresultStatus(relations.get()), PGRES_TUPLES_OK);
    EXPECT_EQ(std::string(PQgetvalue(relations.get(), 0, 0)), "3");
}
TEST_F(RelationalForeignKeys, NullColumnDoesNotHideAnotherRelationshipOnSameOwnerRow) {
    const auto rows = read({node("Comment", "c3")}, RelationshipLookupDirection::outgoing);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].type, reviewed);
    EXPECT_EQ(rows[0].to.node.global_id, node("Person", 1).global_id);
}

TEST_F(RelationalForeignKeys, CatalogFailureAfterDeliveredPrefixRollsBackAndKeepsConnectionUsable) {
    sql("INSERT INTO " + table("post") + " VALUES('00000000-0000-0000-0000-00000000000d',NULL); UPDATE " +
        table("comment") + " SET reply_to_post_id='00000000-0000-0000-0000-00000000000d' WHERE id='c1'");
    RelationalForeignKeyReader reader(mapping(), *identities);
    // Configured Authors precedes ReplyOf: its valid batch is delivered before
    // the next relationship fails, without depending on PostgreSQL row order.
    const std::array<NodeRef, 2> start{node("Person", 1), node("Comment", "c1")};
    std::size_t delivered = 0;
    EXPECT_THROW(reader.stream(db.get(), start,
                               {.relationship_types = {authors, reply},
                                .direction = RelationshipLookupDirection::outgoing,
                                .batch_size = 1},
                               [&](auto rows) {
                                   for (const auto &r: rows)
                                       EXPECT_EQ(r.type, authors);
                                   delivered += rows.size();
                               }),
                 std::invalid_argument);
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(PQtransactionStatus(db.get()), PQTRANS_IDLE);
    EXPECT_EQ(read({node("Person", 1)}, RelationshipLookupDirection::outgoing, {authors}).size(), 1);
}
