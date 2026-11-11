#include <cstdlib>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <libpq-fe.h>
#include <unistd.h>
#include "algorithms/bfs/BfsExecutor.hpp"
#include "cli/BackendConfigParser.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "cli/SourceNodeIdResolver.hpp"
#include "data_access/adapters/PostgreSQL/PostgresRelationalGraphStore.hpp"
#include "data_access/adapters/PostgreSQL/RelationalAttachment.hpp"
#include "domain/graph/LogicalGraphSchemaJson.hpp"
#include "infrastructure/ConfiguredGraphStoreFactory.hpp"
#include "ingestion/InputAdapterRegistry.hpp"
#include "support/TestGraphStore.hpp"
#include "utilities/BackendIds.hpp"

using namespace polygraph;
namespace ing = polygraph::ingestion;
namespace pg = polygraph::postgres;
namespace {
    class SourceRows final : public ing::IGraphInputAdapter {
        std::vector<ing::Record> rows_;

    public:
        explicit SourceRows(std::vector<ing::Record> rows) : rows_(std::move(rows)) {}
        void nodes(const ing::RecordConsumer &c) override {
            for (const auto &r: rows_)
                c(r);
        }
        void relationships(const ing::RecordConsumer &) override {}
        json metadata() const override { return json::object(); }
    };
    class RelationalBfs : public ::testing::Test {
    protected:
        ing::fs::path root;
        std::string database;
        std::string limited_role;
        std::unique_ptr<PGconn, decltype(&PQfinish)> admin{nullptr, PQfinish}, db{nullptr, PQfinish};
        std::shared_ptr<DatabaseContext> context;
        json schema_json, mapping_json, backend;
        pg::RelationalMapping mapping;
        const std::string uuid = "00000000-0000-0000-0000-00000000000a";
        void sql(PGconn *connection, const std::string &text) {
            std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(connection, text.c_str()), PQclear);
            if (!r || (PQresultStatus(r.get()) != PGRES_COMMAND_OK && PQresultStatus(r.get()) != PGRES_TUPLES_OK))
                throw std::runtime_error(r ? PQresultErrorMessage(r.get()) : PQerrorMessage(connection));
        }
        void SetUp() override {
            auto pattern = (ing::fs::temp_directory_path() / "polygraph-relbfs-XXXXXX").string();
            ASSERT_NE(mkdtemp(pattern.data()), nullptr);
            root = pattern;
            database = "relbfs_" + root.filename().string().substr(17);
            admin.reset(PQconnectdb(std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN")));
            ASSERT_EQ(PQstatus(admin.get()), CONNECTION_OK) << PQerrorMessage(admin.get());
            sql(admin.get(), "CREATE DATABASE \"" + database + "\"");
            const char *keys[] = {"host", "port", "user", "dbname", nullptr};
            const char *values[] = {PQhost(admin.get()), PQport(admin.get()), PQuser(admin.get()), database.c_str(),
                                    nullptr};
            db.reset(PQconnectdbParams(keys, values, 0));
            ASSERT_EQ(PQstatus(db.get()), CONNECTION_OK);
            sql(db.get(),
                "CREATE SCHEMA app; CREATE TABLE app.region(id text PRIMARY KEY);"
                "CREATE TABLE app.person(id bigint PRIMARY KEY,manager bigint REFERENCES app.person(id));"
                "CREATE TABLE app.post(id uuid PRIMARY KEY,region text REFERENCES app.region(id));"
                "CREATE TABLE app.likes(person bigint REFERENCES app.person(id),post uuid REFERENCES app.post(id));"
                "CREATE TABLE app.home(region text REFERENCES app.region(id),person bigint REFERENCES app.person(id));"
                "INSERT INTO app.region VALUES('central'); INSERT INTO app.person VALUES(7,NULL),(8,7);"
                "INSERT INTO app.post VALUES('00000000-0000-0000-0000-00000000000a','central');"
                "INSERT INTO app.likes "
                "VALUES(7,'00000000-0000-0000-0000-00000000000a'),(7,'00000000-0000-0000-0000-00000000000a');"
                "INSERT INTO app.home VALUES('central',8)");
            schema_json = {{"nodeTypes", json::array({{{"name", "Person"}}, {{"name", "Post"}}, {{"name", "Region"}}})},
                           {"relations", json::array({{{"name", "LIKES"}, {"from", "Person"}, {"to", "Post"}},
                                                      {{"name", "REGION"}, {"from", "Post"}, {"to", "Region"}},
                                                      {{"name", "HOME"}, {"from", "Region"}, {"to", "Person"}},
                                                      {{"name", "MANAGER"}, {"from", "Person"}, {"to", "Person"}},
                                                      {{"name", "UNMAPPED"}, {"from", "Person"}, {"to", "Person"}}})}};
            context = DatabaseContext::create_empty();
            context->schema = domain::logical_schema_from_json(schema_json);
            context->id_routing_backends = {NEO4J, ARANGO, POSTGRESQL};
            context->placement_catalog->set_id_routing_backends(context->id_routing_backends);
            mapping_json = {
                    {"format", "polygraph.postgresql-relational"},
                    {"version", 1},
                    {"nodes", json::array({{{"type", "Person"},
                                            {"schema", "app"},
                                            {"table", "person"},
                                            {"primaryKey", "id"},
                                            {"namespace", "bfs"},
                                            {"keyCodec", "int64"}},
                                           {{"type", "Post"},
                                            {"schema", "app"},
                                            {"table", "post"},
                                            {"primaryKey", "id"},
                                            {"namespace", "bfs"},
                                            {"keyCodec", "uuid"}},
                                           {{"type", "Region"},
                                            {"schema", "app"},
                                            {"table", "region"},
                                            {"primaryKey", "id"},
                                            {"namespace", "bfs"},
                                            {"keyCodec", "string"}}})},
                    {"relationships",
                     json::array({{{"type", "LIKES"},
                                   {"kind", "table"},
                                   {"schema", "app"},
                                   {"table", "likes"},
                                   {"fromColumn", "person"},
                                   {"toColumn", "post"}},
                                  {{"type", "REGION"}, {"kind", "foreignKey"}, {"owner", "from"}, {"column", "region"}},
                                  {{"type", "HOME"},
                                   {"kind", "table"},
                                   {"schema", "app"},
                                   {"table", "home"},
                                   {"fromColumn", "region"},
                                   {"toColumn", "person"}},
                                  {{"type", "MANAGER"},
                                   {"kind", "foreignKey"},
                                   {"owner", "from"},
                                   {"column", "manager"}}})}};
            mapping = pg::parse_relational_mapping(mapping_json, *context->schema);
            (void) pg::attach_relational_nodes(db.get(), mapping, {.output_dir = root / "catalog"});
            ing::write_json_atomic(root / "mapping.json", mapping_json);
            ing::write_json_atomic(root / "schema.json", schema_json);
            backend = {{"id", "PostgreSQL"},
                       {"kind", "PostgreSQL"},
                       {"relationalMapping", "mapping.json"},
                       {"relationalIdentity", "catalog/manifest.json"},
                       {"connection",
                        {{"endpoint", std::string(PQhost(admin.get())) + ":" + PQport(admin.get())},
                         {"database", database},
                         {"credentials", {{"username", PQuser(admin.get())}, {"password", ""}}}}},
                       {"capabilities", {{"batch_neighbors", true}}},
                       {"coverage", {{"mode", "id-routing"}, {"fresh", true}}}};
        }
        void TearDown() override {
            context.reset();
            db.reset();
            if (admin && PQstatus(admin.get()) == CONNECTION_OK && !database.empty())
                sql(admin.get(), "DROP DATABASE IF EXISTS \"" + database + "\" WITH(FORCE)");
            if (admin && PQstatus(admin.get()) == CONNECTION_OK && !limited_role.empty())
                sql(admin.get(), "DROP ROLE \"" + limited_role + "\"");
            if (!root.empty())
                ing::fs::remove_all(root);
        }
        NodeRef node(const std::string &type, json key, const std::string &catalog = "catalog") {
            const pg::RelationalIdentityResolver ids(root / catalog / "manifest.json", mapping);
            const std::array<domain::SourceIdentity, 1> source{{{"bfs", type, key}}};
            return ids.resolve(source)[0].node;
        }
        std::shared_ptr<IGraphStore> store(bool dedup = false) {
            backend["server_side_candidate_deduplication"] = dedup;
            ing::write_json_atomic(root / "backends.json", {{"idRouting", {"Neo4J", "ArangoDB", "PostgreSQL"}},
                                                            {"backends", json::array({backend})}});
            const auto config = config::load_backend_config_file_or_default((root / "backends.json").string());
            const std::unordered_map<BackendId, std::unordered_set<TypeId>> types;
            ConfiguredGraphStoreFactory::Inputs inputs{
                    .data_path = "", .schema = *context->schema, .nodes_by_backend = types, .edges_by_backend = types};
            return ConfiguredGraphStoreFactory::create_from_json(*context, config["backends"][0], inputs);
        }
        ExpandSpec directed() { return {.direction = Direction::Directed}; }
    };
} // namespace
TEST_F(RelationalBfs, RuntimeFactoryExpandsAttachedKeysAcrossTableAndForeignKeyMappings) {
    auto graph = store();
    ASSERT_NE(dynamic_cast<PostgresRelationalGraphStore *>(graph.get()), nullptr);
    const auto root_id =
            cli::SourceNodeIdResolver(root / "catalog/manifest.json").resolve({"bfs", "Person", std::int64_t{7}}).id;
    const std::array<NodeRef, 1> frontier{{{root_id}}};
    const auto rich = graph->get_neighbors_batch(frontier, directed(), 0);
    ASSERT_EQ(rich.size(), 1);
    EXPECT_EQ(rich[0].neighbors.size(), 2);
    const auto post = node("Post", uuid);
    for (const auto &n: rich[0].neighbors)
        EXPECT_EQ(n.vertex.global_id, post.global_id);
    const auto minimal = graph->get_minimal_neighbors_batch(std::array{post}, directed(), 1, {});
    ASSERT_EQ(minimal.size(), 1);
    ASSERT_EQ(minimal[0].neighbors.size(), 1);
    EXPECT_EQ(minimal[0].neighbors[0].global_id, node("Region", "central").global_id);
}
TEST_F(RelationalBfs, BfsDistancesPredecessorsAndStreamedCandidatesAgree) {
    context->register_store(store());
    BfsExecutor bfs(*context->placement_catalog);
    const auto p7 = node("Person", std::int64_t{7}), p8 = node("Person", std::int64_t{8}), post = node("Post", uuid),
               region = node("Region", "central");
    BfsOptions options;
    options.allow_native_pushdown = false;
    options.neighbor_batch_size = 2;
    options.streaming_candidate_batch_size = 1;
    const auto result = bfs.run(p7, directed(), options);
    EXPECT_EQ(result.distance,
              (decltype(result.distance){
                      {p7.global_id, 0}, {post.global_id, 1}, {region.global_id, 2}, {p8.global_id, 3}}));
    EXPECT_EQ(result.predecessor.at(post.global_id), p7.global_id);
    EXPECT_EQ(result.predecessor.at(region.global_id), post.global_id);
    EXPECT_EQ(result.predecessor.at(p8.global_id), region.global_id);
    options.return_predecessors = false;
    EXPECT_EQ(bfs.run(p7, directed(), options).distance, result.distance);
    options.max_depth = 2;
    EXPECT_EQ(bfs.run(p7, directed(), options).distance.size(), 3);
}
TEST_F(RelationalBfs, FiltersBothDirectionsAndEmptyFrontiersKeepInterfaceSemantics) {
    auto graph = store();
    const auto p7 = node("Person", std::int64_t{7});
    auto spec = directed();
    spec.allowed_edge_types = {context->schema->edge_type_id("UNMAPPED")};
    EXPECT_TRUE(graph->get_minimal_neighbors_batch(std::array{p7}, spec, 0, {})[0].neighbors.empty());
    spec.allowed_edge_types = {context->schema->edge_type_id("LIKES")};
    spec.allowed_target_node_types = {context->schema->node_type_id("Region")};
    EXPECT_TRUE(graph->get_minimal_neighbors_batch(std::array{p7}, spec, 0, {})[0].neighbors.empty());
    spec = {.direction = Direction::Both};
    EXPECT_EQ(graph->get_minimal_neighbors_batch(std::array{p7}, spec, 0, {})[0].neighbors.size(), 3);
    EXPECT_TRUE(graph->get_neighbors_batch({}, spec, 0).empty());
}
TEST_F(RelationalBfs, OptionalCandidateDeduplicationPreservesParityAndBoundedCallbacks) {
    auto graph = store(true);
    const auto p7 = node("Person", std::int64_t{7});
    EXPECT_EQ(graph->get_minimal_neighbors_batch(std::array{p7}, directed(), 0, {})[0].neighbors.size(), 1);
    BfsOptions options;
    options.streaming_candidate_batch_size = 1;
    unsigned total = 0;
    graph->stream_neighbor_candidates(std::array{p7}, directed(), 0, options, [&](auto batch) {
        EXPECT_LE(batch.size(), 1);
        total += batch.size();
    });
    EXPECT_EQ(total, 1);
}
TEST_F(RelationalBfs, FullRuntimeBuilderLoadsAttachedDatabaseAndSourceRoot) {
    (void) store();
    auto runtime_config = ing::read_json(root / "backends.json");
    runtime_config["idRouting"] = {"PostgreSQL"};
    ing::write_json_atomic(root / "backends.json", runtime_config);
    std::ofstream(root / "graph.jsonl") << json{{"kind", "metadata"},
                                                {"idRouting", {"PostgreSQL"}},
                                                {"idLocationEncoding", "bitmask-msb-v1"},
                                                {"idLocationBits", 16},
                                                {"idPayloadBits", 48}}
                                                   .dump()
                                        << '\n';
    auto runtime = cli::GraphRuntimeBuilder::build({.logical_graph_schema_path = (root / "schema.json").string(),
                                                    .data_path = (root / "graph.jsonl").string(),
                                                    .backend_config_path = (root / "backends.json").string()});
    const auto source =
            cli::SourceNodeIdResolver(root / "catalog/manifest.json").resolve({"bfs", "Person", std::int64_t{7}});
    EXPECT_EQ(source.owner, POSTGRESQL);
    BfsExecutor bfs(*runtime->placement_catalog);
    EXPECT_EQ(bfs.run(runtime->node_ref(source.id), directed(), {}).distance.size(), 4);
}
TEST_F(RelationalBfs, GenericPostgresqlAndRelationalBfsHaveIdenticalDistances) {
    auto relational = store();
    context->register_store(relational);
    const auto p7 = node("Person", std::int64_t{7}), p8 = node("Person", std::int64_t{8}), post = node("Post", uuid),
               region = node("Region", "central");
    sql(db.get(),
        "CREATE TABLE public.polygraph_edges(from_id bigint,to_id bigint,from_type text,to_type text,edge_type text)");
    const auto edge = [&](NodeRef from, NodeRef to, const std::string &type, const std::string &from_type,
                          const std::string &to_type) {
        sql(db.get(), "INSERT INTO public.polygraph_edges VALUES(" + std::to_string(ing::signed_id(from.global_id)) +
                              "," + std::to_string(ing::signed_id(to.global_id)) + ",'" + from_type + "','" + to_type +
                              "','" + type + "')");
    };
    edge(p7, post, "LIKES", "Person", "Post");
    edge(p7, post, "LIKES", "Person", "Post");
    edge(post, region, "REGION", "Post", "Region");
    edge(region, p8, "HOME", "Region", "Person");
    edge(p8, p7, "MANAGER", "Person", "Person");
    auto generic_backend = backend;
    generic_backend.erase("relationalMapping");
    generic_backend.erase("relationalIdentity");
    const std::unordered_map<BackendId, std::unordered_set<TypeId>> types;
    const ConfiguredGraphStoreFactory::Inputs inputs{
            .data_path = "", .schema = *context->schema, .nodes_by_backend = types, .edges_by_backend = types};
    auto generic = ConfiguredGraphStoreFactory::create_from_json(*context, generic_backend, inputs);
    auto registry = std::make_shared<BackendRegistry>();
    registry->register_store(generic);
    PlacementCatalog catalog(registry);
    catalog.set_id_routing_backends(context->id_routing_backends);
    BfsExecutor left(*context->placement_catalog), right(catalog);
    for (const auto direction: {Direction::Directed, Direction::Both, Direction::Undirected}) {
        ExpandSpec spec{.direction = direction};
        EXPECT_EQ(left.run(p7, spec, {}).distance, right.run(p7, spec, {}).distance);
        BfsOptions streamed;
        streamed.return_predecessors = false;
        EXPECT_EQ(left.run(p7, spec, streamed).distance, right.run(p7, spec, streamed).distance);
    }
}
TEST_F(RelationalBfs, DistributedBfsHandsOffPostgresqlNeo4jArangoAndBackToPostgresql) {
    const std::vector<ing::Record> rows{
            {.type = "Person", .identity = ing::SourceIdentity{"bfs", "Person", std::int64_t{7}}},
            {.type = "Person", .identity = ing::SourceIdentity{"bfs", "Person", std::int64_t{8}}},
            {.type = "Post", .identity = ing::SourceIdentity{"bfs", "Post", uuid}},
            {.type = "Region", .identity = ing::SourceIdentity{"bfs", "Region", "central"}}};
    ing::InputAdapterRegistry adapters;
    adapters.add({.name = "bfs-test",
                  .factory = [rows](const auto &, const auto &) { return std::make_unique<SourceRows>(rows); },
                  .input_files = [](const auto &o) { return std::vector<ing::fs::path>{o.input}; },
                  .id_policy = ing::InputIdPolicy::typed_external,
                  .version = "1"});
    ing::write_json_atomic(root / "source.json", {{"fixture", 1}});
    ing::write_json_atomic(root / "placement.json",
                           {{"storageBackends", {"PostgreSQL", "Neo4J", "ArangoDB"}},
                            {"nodeSources", {{"Person", "PostgreSQL"}, {"Post", "Neo4J"}, {"Region", "ArangoDB"}}},
                            {"defaultRelationshipSource", "PostgreSQL"}});
    (void) ing::prepare({.adapter = "bfs-test",
                         .input = root / "source.json",
                         .placement_config = root / "placement.json",
                         .output_dir = root / "prepared",
                         .input_adapters = &adapters});
    (void) pg::attach_relational_nodes(
            db.get(), mapping, {.output_dir = root / "mixed", .source_manifest = root / "prepared/manifest.json"});
    backend["relationalIdentity"] = "mixed/manifest.json";
    context->register_store(store());
    const auto p7 = node("Person", std::int64_t{7}, "mixed"), p8 = node("Person", std::int64_t{8}, "mixed"),
               post = node("Post", uuid, "mixed"), region = node("Region", "central", "mixed");
    EXPECT_EQ(post.global_id, ing::make_id(ing::Backend::neo4j, 2));
    EXPECT_EQ(region.global_id, ing::make_id(ing::Backend::arango, 3));
    auto neo = std::make_shared<test::TestGraphStore>(test::descriptor(NEO4J, 10, BackendKind::Neo4j),
                                                      test::TestGraphStore::Graph{{post.global_id, {region}}});
    auto arango = std::make_shared<test::TestGraphStore>(test::descriptor(ARANGO, 10, BackendKind::Arango),
                                                         test::TestGraphStore::Graph{{region.global_id, {p8}}});
    context->register_store(neo);
    context->register_store(arango);
    BfsExecutor bfs(*context->placement_catalog);
    const auto result = bfs.run(p7, directed(), {});
    EXPECT_EQ(result.distance,
              (decltype(result.distance){
                      {p7.global_id, 0}, {post.global_id, 1}, {region.global_id, 2}, {p8.global_id, 3}}));
    EXPECT_GT(neo->neighbor_call_count(), 0);
    EXPECT_GT(arango->neighbor_call_count(), 0);
    BfsOptions streamed;
    streamed.return_predecessors = false;
    EXPECT_EQ(bfs.run(p7, directed(), streamed).distance, result.distance);
}
TEST_F(RelationalBfs, CallbackFailureAndReentrancyLeaveStoreReusable) {
    auto graph = store();
    const auto p7 = node("Person", std::int64_t{7});
    BfsOptions options;
    options.streaming_candidate_batch_size = 1;
    EXPECT_THROW(graph->stream_neighbor_candidates(std::array{p7}, directed(), 0, options,
                                                   [](auto) { throw std::runtime_error("cancel"); }),
                 std::runtime_error);
    EXPECT_EQ(graph->get_minimal_neighbors_batch(std::array{p7}, directed(), 0, {})[0].neighbors.size(), 2);
    bool nested = false;
    graph->stream_neighbor_candidates(std::array{p7}, directed(), 0, options, [&](auto) {
        if (!nested) {
            nested = true;
            EXPECT_EQ(graph->get_minimal_neighbors_batch(std::array{p7}, directed(), 0, {})[0].neighbors.size(), 2);
        }
    });
    EXPECT_TRUE(nested);
}
TEST_F(RelationalBfs, StaleFrontierFailsEvenWhenFiltersSelectNoMappedEdges) {
    auto graph = store();
    const auto p7 = node("Person", std::int64_t{7});
    sql(db.get(), "DELETE FROM app.likes;UPDATE app.person SET manager=NULL;DELETE FROM app.person WHERE id=7");
    auto spec = directed();
    spec.allowed_edge_types = {context->schema->edge_type_id("UNMAPPED")};
    EXPECT_THROW((void) graph->get_minimal_neighbors_batch(std::array{p7}, spec, 0, {}), std::exception);
    sql(db.get(), "INSERT INTO app.person VALUES(7,NULL)");
    EXPECT_TRUE(graph->get_minimal_neighbors_batch(std::array{p7}, spec, 0, {})[0].neighbors.empty());
}
TEST_F(RelationalBfs, DuplicateAndUnresolvedFrontierRowsKeepExactSourceAssociation) {
    auto graph = store();
    const auto p7 = node("Person", std::int64_t{7}), p8 = node("Person", std::int64_t{8});
    const std::array<NodeRef, 3> frontier{{{p7.global_id}, p8, p7}};
    const auto result = graph->get_minimal_neighbors_batch(frontier, directed(), 0, {});
    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0].neighbors.size(), 2);
    EXPECT_EQ(result[1].neighbors.size(), 1);
    EXPECT_EQ(result[2].neighbors.size(), 2);
    EXPECT_EQ(result[1].neighbors[0].global_id, p7.global_id);
}
TEST_F(RelationalBfs, LargeFrontiersSplitWithoutLosingSourcesOrCallbackBounds) {
    sql(db.get(), "INSERT INTO app.person(id) SELECT generate_series(100,4299)");
    (void) pg::attach_relational_nodes(
            db.get(), mapping, {.output_dir = root / "large", .previous_manifest = root / "catalog/manifest.json"});
    backend["relationalIdentity"] = "large/manifest.json";
    auto graph = store();
    pg::RelationalIdentityResolver ids(root / "large/manifest.json", mapping);
    std::vector<domain::SourceIdentity> keys{{"bfs", "Person", std::int64_t{7}}, {"bfs", "Person", std::int64_t{8}}};
    for (std::int64_t i = 100; i <= 4299; ++i)
        keys.push_back({"bfs", "Person", i});
    std::vector<NodeRef> frontier;
    for (std::size_t start = 0; start < keys.size(); start += 4096)
        for (const auto &n:
             ids.resolve(std::span(keys).subspan(start, std::min<std::size_t>(4096, keys.size() - start))))
            frontier.push_back(n.node);
    const auto rows = graph->get_minimal_neighbors_batch(frontier, directed(), 0, {});
    EXPECT_EQ(rows.size(), 4202);
    std::size_t total = 0;
    for (const auto &row: rows)
        total += row.neighbors.size();
    EXPECT_EQ(total, 3);
    BfsOptions options;
    options.streaming_candidate_batch_size = 2;
    total = 0;
    graph->stream_neighbor_candidates(frontier, directed(), 0, options, [&](auto batch) {
        EXPECT_LE(batch.size(), 2);
        total += batch.size();
    });
    EXPECT_EQ(total, 3);
}
TEST_F(RelationalBfs, ConcurrentRequestsDoNotShareActiveLibpqConnections) {
    auto graph = store();
    const auto p7 = node("Person", std::int64_t{7});
    std::vector<std::future<std::size_t>> calls;
    for (unsigned i = 0; i < 6; ++i)
        calls.push_back(std::async(std::launch::async, [graph, p7] {
            return graph->get_minimal_neighbors_batch(std::array{p7}, {.direction = Direction::Directed}, 0, {})[0]
                    .neighbors.size();
        }));
    for (auto &call: calls)
        EXPECT_EQ(call.get(), 2);
    EXPECT_EQ(graph->get_minimal_neighbors_batch(std::array{p7}, directed(), 0, {})[0].neighbors.size(), 2);
}
TEST_F(RelationalBfs, SelectOnlyRoleNeedsNoPostgresqlTempOrApplicationWritePermissions) {
    limited_role = database + "_reader";
    sql(db.get(), "CREATE ROLE \"" + limited_role + "\" LOGIN;GRANT USAGE ON SCHEMA app TO \"" + limited_role +
                          "\";GRANT SELECT ON ALL TABLES IN SCHEMA app TO \"" + limited_role +
                          "\";REVOKE TEMPORARY ON DATABASE \"" + database + "\" FROM PUBLIC");
    backend["connection"]["credentials"]["username"] = limited_role;
    auto graph = store(true);
    const auto p7 = node("Person", std::int64_t{7});
    EXPECT_EQ(graph->get_minimal_neighbors_batch(std::array{p7}, directed(), 0, {})[0].neighbors.size(), 1);
    std::size_t total = 0;
    graph->stream_neighbor_candidates(std::array{p7}, directed(), 0, {}, [&](auto batch) { total += batch.size(); });
    EXPECT_EQ(total, 1);
}
TEST_F(RelationalBfs, InvalidFiltersBatchesAndTypeMismatchesFailWithoutPoisoningConnection) {
    auto graph = store();
    auto p7 = node("Person", std::int64_t{7});
    auto spec = directed();
    spec.allowed_edge_types = {999999};
    EXPECT_THROW((void) graph->get_neighbors_batch(std::array{p7}, spec, 0), std::exception);
    BfsOptions options;
    options.neighbor_batch_size = 0;
    EXPECT_THROW((void) graph->get_minimal_neighbors_batch(std::array{p7}, directed(), 0, options), std::exception);
    auto wrong = p7;
    wrong.type = context->schema->node_type_id("Post");
    EXPECT_THROW((void) graph->get_neighbors_batch(std::array{wrong}, directed(), 0), std::exception);
    EXPECT_EQ(graph->get_neighbors_batch(std::array{p7}, directed(), 0)[0].neighbors.size(), 2);
}
int main(int argc, char **argv) {
    if (!std::getenv("POLYGRAPH_RELATIONAL_TEST_DSN"))
        return 77;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
