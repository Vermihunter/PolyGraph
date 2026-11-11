#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <libpq-fe.h>
#include <map>
#include <set>
#include "algorithms/bfs/BfsExecutor.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "cli/SourceNodeIdResolver.hpp"
#include "data_access/adapters/PostgreSQL/PostgresRelationalNodeReader.hpp"
#include "utilities/BackendIds.hpp"

using namespace polygraph;
namespace pg = polygraph::postgres;
namespace {
    std::filesystem::path scenario_path;
    json read_json(const std::filesystem::path &path) {
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("Cannot read acceptance fixture: " + path.string());
        return json::parse(input);
    }
    class RelationalAcceptance : public ::testing::Test {
    protected:
        json scenario, oracle;
        std::shared_ptr<DatabaseContext> context;
        std::map<std::string, NodeRef> nodes;
        void SetUp() override {
            scenario = read_json(scenario_path);
            oracle = read_json(scenario.at("oracle").get<std::string>());
            context = cli::GraphRuntimeBuilder::build({.logical_graph_schema_path = scenario.at("schema"),
                                                       .data_path = scenario.at("data"),
                                                       .backend_config_path = scenario.at("backends")});
            const cli::SourceNodeIdResolver resolver(scenario.at("catalog").get<std::string>());
            for (const auto &[alias, selector]: oracle.at("nodes").items()) {
                const auto source = domain::source_identity_from_json(selector);
                const auto resolved = resolver.resolve(source);
                nodes.emplace(alias, NodeRef{resolved.id, context->schema->node_type_id(source.type)});
                EXPECT_EQ(context->placement_catalog->owner_backend(resolved.id), resolved.owner);
            }
        }
        ExpandSpec spec(const json &test) const {
            ExpandSpec result;
            const auto direction = test.at("direction").get<std::string>();
            result.direction = direction == "directed"     ? Direction::Directed
                               : direction == "undirected" ? Direction::Undirected
                                                           : Direction::Both;
            for (const auto &name: test.value("edgeTypes", json::array()))
                result.allowed_edge_types.push_back(context->schema->edge_type_id(name.get<std::string>()));
            for (const auto &name: test.value("targetTypes", json::array()))
                result.allowed_target_node_types.push_back(context->schema->node_type_id(name.get<std::string>()));
            return result;
        }
        std::multiset<GlobalId> neighbors(const std::string &alias, const ExpandSpec &filter) const {
            std::multiset<GlobalId> result;
            for (const auto &edge: oracle.at("edges")) {
                const auto type = context->schema->edge_type_id(edge[0].get<std::string>());
                if (!filter.allowed_edge_types.empty() &&
                    std::ranges::find(filter.allowed_edge_types, type) == filter.allowed_edge_types.end())
                    continue;
                const auto from = edge[1].get<std::string>(), to = edge[2].get<std::string>();
                const auto add = [&](const std::string &target) {
                    const auto node = nodes.at(target);
                    if (filter.allowed_target_node_types.empty() ||
                        std::ranges::find(filter.allowed_target_node_types, node.type) !=
                                filter.allowed_target_node_types.end())
                        result.insert(node.global_id);
                };
                if (alias == from)
                    add(to);
                if (filter.direction != Direction::Directed && alias == to && from != to)
                    add(from);
            }
            return result;
        }
    };
} // namespace

TEST_F(RelationalAcceptance, TypedRootsPreserveDistinctNativeKeysAndAuthoritativeOwners) {
    std::set<GlobalId> ids;
    json resolved = json::object();
    for (const auto &[alias, node]: nodes) {
        EXPECT_TRUE(ids.insert(node.global_id).second) << alias;
        resolved[alias] = node.global_id;
    }
    const bool mixed = context->backend_registry->stores().size() == 3;
    EXPECT_EQ(context->placement_catalog->owner_backend(nodes.at("gateway").global_id), mixed ? NEO4J : POSTGRESQL);
    EXPECT_EQ(context->placement_catalog->owner_backend(nodes.at("sensor").global_id), mixed ? ARANGO : POSTGRESQL);
    std::ofstream output(scenario_path.parent_path() / "resolved-ids.json");
    output << resolved.dump(2) << '\n';
    ASSERT_TRUE(output.good());
}

TEST_F(RelationalAcceptance, TypedPropertyReaderPreservesNullUnicodeAndIntegerPrecision) {
    const auto mapping =
            pg::parse_relational_mapping(read_json(scenario.at("mapping").get<std::string>()), *context->schema);
    const pg::RelationalIdentityResolver identities(scenario.at("catalog").get<std::string>(), mapping);
    std::unique_ptr<PGconn, decltype(&PQfinish)> db(PQconnectdb(""), PQfinish);
    ASSERT_EQ(PQstatus(db.get()), CONNECTION_OK) << PQerrorMessage(db.get());
    const pg::PostgresRelationalNodeReader reader(mapping, identities);
    const auto records = reader.read_nodes(db.get(), std::array{nodes.at("p7"), nodes.at("p8"), nodes.at("empty")});
    ASSERT_EQ(records.size(), 3);
    const auto &first = records[0]->fields[0].as<Node>();
    EXPECT_EQ(first.properties.at("name")->as<std::string>(), "Zoë 雪");
    EXPECT_EQ(first.properties.at("total")->as<std::int64_t>(), INT64_MAX);
    EXPECT_TRUE(first.properties.at("active")->as<bool>());
    const auto &second = records[1]->fields[0].as<Node>();
    EXPECT_TRUE(second.properties.at("name")->is_null());
    EXPECT_EQ(second.properties.at("total")->as<std::int64_t>(), INT64_MIN);
    EXPECT_FALSE(second.properties.at("active")->as<bool>());
    EXPECT_TRUE(records[2]->fields[0].as<Node>().properties.at("title")->is_null());
}

TEST_F(RelationalAcceptance, RichMinimalAndBoundedStreamsMatchLiteralEdgesInEveryDirection) {
    BfsOptions options;
    options.neighbor_batch_size = 2;
    options.streaming_candidate_batch_size = 1;
    for (const auto direction: {Direction::Directed, Direction::Both, Direction::Undirected}) {
        const ExpandSpec filter{.direction = direction};
        for (const auto &[alias, node]: nodes) {
            SCOPED_TRACE(alias + " direction=" + std::to_string(static_cast<int>(direction)));
            const auto store =
                    context->backend_registry->get_store(context->placement_catalog->owner_backend(node.global_id));
            const std::array frontier{node};
            const auto want = neighbors(alias, filter);
            const auto rich = store->get_neighbors_batch(frontier, filter, 0);
            std::multiset<GlobalId> actual;
            for (const auto &row: rich)
                for (const auto &neighbor: row.neighbors) {
                    actual.insert(neighbor.vertex.global_id);
                    EXPECT_NE(neighbor.vertex.type, unresolved_type_id);
                }
            EXPECT_EQ(actual, want);
            actual.clear();
            for (const auto &row: store->get_minimal_neighbors_batch(frontier, filter, 0, options))
                for (const auto &neighbor: row.neighbors)
                    actual.insert(neighbor.global_id);
            EXPECT_EQ(actual, want);
            actual.clear();
            store->stream_neighbor_candidates(frontier, filter, 0, options, [&](std::span<const NodeRef> batch) {
                EXPECT_LE(batch.size(), 1);
                for (const auto &neighbor: batch)
                    actual.insert(neighbor.global_id);
            });
            EXPECT_EQ(actual, want);
        }
    }
}

TEST_F(RelationalAcceptance, BfsMatchesHandCheckedLayersAndValidPredecessorsAcrossOwners) {
    BfsExecutor executor(*context->placement_catalog);
    for (const auto &test: oracle.at("cases")) {
        SCOPED_TRACE(test.at("name").get<std::string>());
        BfsResult expected;
        std::uint32_t depth = 0;
        for (const auto &layer: test.at("layers")) {
            for (const auto &alias: layer)
                expected.distance.emplace(nodes.at(alias.get<std::string>()).global_id, depth);
            ++depth;
        }
        const auto root = nodes.at(test.at("root").get<std::string>());
        const auto filter = spec(test);
        BfsOptions options;
        options.allow_native_pushdown = false;
        options.neighbor_batch_size = 2;
        options.streaming_candidate_batch_size = 1;
        options.max_depth = test.value("maxDepth", -1);
        const auto result = executor.run(root, filter, options);
        EXPECT_EQ(result.distance, expected.distance);
        for (const auto &[alias, node]: nodes) {
            if (node.global_id == root.global_id || !expected.distance.contains(node.global_id))
                continue;
            ASSERT_TRUE(result.predecessor.contains(node.global_id)) << alias;
            const auto parent = result.predecessor.at(node.global_id);
            ASSERT_TRUE(result.distance.contains(parent));
            EXPECT_EQ(result.distance.at(parent) + 1, result.distance.at(node.global_id));
            const auto parent_node =
                    std::ranges::find_if(nodes, [&](const auto &entry) { return entry.second.global_id == parent; });
            ASSERT_NE(parent_node, nodes.end());
            EXPECT_TRUE(neighbors(parent_node->first, filter).contains(node.global_id));
        }
        options.return_predecessors = false;
        const auto streamed = executor.run(root, filter, options);
        EXPECT_EQ(streamed.distance, expected.distance);
        EXPECT_TRUE(streamed.predecessor.empty());
    }
}

TEST_F(RelationalAcceptance, WrongGenerationAndUnknownMappingAreRejected) {
    const auto document = read_json(scenario.at("mapping").get<std::string>());
    const auto mapping = pg::parse_relational_mapping(document, *context->schema);
    EXPECT_THROW((void) pg::RelationalIdentityResolver(scenario.at("catalog").get<std::string>(), mapping,
                                                       "wrong-generation"),
                 std::exception);
    auto wrong = document;
    wrong["nodes"][0]["type"] = "UNKNOWN";
    EXPECT_THROW((void) pg::parse_relational_mapping(wrong, *context->schema), std::exception);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Requires an owned relational_live.py scenario.json; missing input is a failure, not a skip\n";
        return 2;
    }
    scenario_path = argv[1];
    argc = 1;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
