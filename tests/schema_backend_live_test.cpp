#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "cli/BackendConfigParser.hpp"
#include "data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp"
#include "data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp"
#include "insertion/InsertionItem.hpp"
#include "query/query_types/GetAllEdgesQuery.hpp"
#include "query/query_types/GetAllVerticesQuery.hpp"
#include "query/query_types/GetNeighborsBatchQuery.hpp"
#include "query/query_types/GetVertexByIdQuery.hpp"
#include "query/query_types/InsertBatchQuery.hpp"
#include "utilities/BackendIds.hpp"

namespace polygraph {
    // Explicitly opt in only against the disposable fixture environment created by docker_load.py.
    TEST(SchemaBackendLiveTest, UnfamiliarSchemaReadsAndWrites) {
        const auto root = std::getenv("POLYGRAPH_SCHEMA_LIVE_ROOT");
        if (!root)
            GTEST_SKIP() << "Requires owned disposable schema_agnostic Docker import";
        const auto directory = std::filesystem::path(root);
        json state;
        std::ifstream(directory / "environment.json") >> state;
        ASSERT_TRUE(state.contains("generation"));
        json config;
        std::ifstream(directory / "runtime.secret.json") >> config;
        // Separate reruns without changing the insertion API's duplicate-write semantics.
        const auto payload = static_cast<GlobalId>(std::chrono::steady_clock::now().time_since_epoch().count()) &
                             ((GlobalId{1} << 40) - 1);
        auto context = DatabaseContext::create_empty();
        context->id_routing_backends = {NEO4J, ARANGO, POSTGRESQL};
        auto &schema = *context->schema;
        const auto device = schema.add_node_type("Device-unit");
        const auto site = schema.add_node_type("Site/office");
        const auto gateway = schema.add_node_type("Gateway:router");
        const auto sensor = schema.add_node_type("Sensorµ");
        const auto installed = schema.add_edge_type("INSTALLED-AT", device, site);
        const auto reports = schema.add_edge_type("REPORTS:TO", sensor, gateway);
        schema.add_edge_type("CONNECTS.to", gateway, device);
        const auto loop = schema.add_edge_type("LOOPS.µ", site, site);
        const auto hosts = schema.add_edge_type("HOSTS/at", site, gateway);
        for (const auto &backend: config::backend_array_or_throw(config)) {
            const auto name = backend.at("id").get<std::string>();
            const auto id = config::backend_id_from_string(name);
            if (id != NEO4J && id != ARANGO)
                continue;
            const auto kind = config::backend_kind_from_string(backend.at("kind").get<std::string>());
            const auto connection = config::parse_connection_config(backend, id, kind);
            const auto scheduler = std::make_shared<BackendRequestScheduler>(name, BackendSchedulerConfig{});
            std::unique_ptr<QueryVisitor> visitor;
            if (id == NEO4J)
                visitor = std::make_unique<Neo4JQueryVisitor>(*context, connection, scheduler);
            else
                visitor = std::make_unique<ArangoQueryVisitor>(*context, connection, scheduler);
            // Exercise physical source names and logical property filters in the fast paths.
            const auto check_minimal = [&](const auto &rows, GlobalId expected, TypeId expected_type) {
                std::size_t count = 0;
                for (const auto &row: rows)
                    for (const auto &neighbor: row.neighbors) {
                        EXPECT_EQ(neighbor.global_id, expected);
                        EXPECT_EQ(neighbor.type, expected_type);
                        ++count;
                    }
                EXPECT_EQ(count, 1);
            };
            const auto neighbor_count = [](const auto &rows) {
                std::size_t count = 0;
                for (const auto &row: rows)
                    count += row.neighbors.size();
                return count;
            };
            if (auto *neo = dynamic_cast<Neo4JQueryVisitor *>(visitor.get())) {
                const std::vector<NodeRef> frontier{{.global_id = 281474976710657ULL, .type = device}};
                ExpandSpec filter{.allowed_edge_types = {installed},
                                  .allowed_target_node_types = {site},
                                  .direction = Direction::Directed,
                                  .return_edge_refs = true};
                const auto rich = neo->get_neighbors_batch_fast(frontier, filter, 0);
                EXPECT_GT(neighbor_count(rich), 0);
                for (const auto &row: rich)
                    for (const auto &neighbor: row.neighbors) {
                        EXPECT_EQ(neighbor.vertex.global_id, 562949953421314ULL);
                        EXPECT_EQ(neighbor.vertex.type, site);
                        ASSERT_TRUE(neighbor.via_edge.has_value());
                        EXPECT_EQ(neighbor.via_edge->type, installed);
                    }
                const auto minimal = [&](const auto &refs) {
                    return neo->get_minimal_neighbors_batch_fast(refs, filter, 0, 100, 1, 0, 0, {});
                };
                check_minimal(minimal(frontier), 562949953421314ULL, site);
                const std::vector<NodeRef> unresolved{{.global_id = frontier[0].global_id}};
                check_minimal(minimal(unresolved), 562949953421314ULL, site);
                filter.allowed_target_node_types = {sensor};
                EXPECT_EQ(neighbor_count(minimal(frontier)), 0);
                EXPECT_EQ(neighbor_count(neo->get_neighbors_batch_fast(frontier, filter, 0)), 0);
                filter.allowed_target_node_types = {site};
                filter.allowed_edge_types = {reports};
                EXPECT_EQ(neighbor_count(minimal(frontier)), 0);
            } else {
                auto *arango = dynamic_cast<ArangoQueryVisitor *>(visitor.get());
                ASSERT_NE(arango, nullptr);
                // Native source with a PostgreSQL-owned proxy target.
                const std::vector<NodeRef> frontier{{.global_id = 562949953421314ULL, .type = site}};
                ExpandSpec filter{.allowed_edge_types = {hosts},
                                  .allowed_target_node_types = {gateway},
                                  .direction = Direction::Directed};
                for (const auto mode: {ArangoExpandMode::Traversal, ArangoExpandMode::EdgeIndex}) {
                    const auto minimal = [&](const auto &refs) {
                        return arango->get_minimal_neighbors_batch_fast(
                                refs, filter, 0, 100, 1, ArangoRequestStrategy::Single, mode, false, 100, 0, 0, {});
                    };
                    check_minimal(minimal(frontier), 9223372036854775811ULL, gateway);
                    filter.allowed_target_node_types = {device};
                    EXPECT_EQ(neighbor_count(minimal(frontier)), 0);
                    filter.allowed_target_node_types = {gateway};
                    filter.allowed_edge_types = {installed};
                    EXPECT_EQ(neighbor_count(minimal(frontier)), 0);
                    filter.allowed_edge_types = {hosts};
                }
            }
            ASSERT_FALSE(visitor->visit(GetAllVerticesQuery{})->data.empty());
            ASSERT_FALSE(visitor->visit(GetAllEdgesQuery{})->data.empty());
            const auto type = id == NEO4J ? device : site;
            const GlobalId base = id == NEO4J ? 281474976710656ULL : 562949953421312ULL;
            Node existing{.global_id = base + (id == NEO4J ? 1 : 2), .type = type};
            ASSERT_EQ(visitor->visit(GetVertexByIdQuery{existing})->data.size(), 1);
            // Insert local nodes, and an edge using physical endpoint collections/labels.
            const auto first = std::make_shared<Node>(Node{.global_id = base + payload, .type = type});
            const auto second_type = id == NEO4J ? site : type;
            const auto second = std::make_shared<Node>(Node{.global_id = base + payload + 1, .type = second_type});
            (void) visitor->visit(
                    InsertBatchQuery{{insertion::InsertionItem{first}, insertion::InsertionItem{second}}});
            auto edge = std::make_shared<Edge>(Edge{.global_id = base + payload + 2,
                                                    .type = id == NEO4J ? installed : loop,
                                                    .src = first->global_id,
                                                    .dst = second->global_id});
            (void) visitor->visit(InsertBatchQuery{{insertion::InsertionItem{edge}}});
            Node inserted{.global_id = first->global_id, .type = type};
            EXPECT_EQ(visitor->visit(GetVertexByIdQuery{inserted})->data.size(), 1);
            const ExpandSpec spec{.allowed_edge_types = {edge->type},
                                  .allowed_target_node_types = {second_type},
                                  .direction = Direction::Directed,
                                  .return_edge_refs = true};
            const auto adjacency = visitor->visit(
                    GetNeighborsBatchQuery{{NodeRef{.global_id = first->global_id, .type = type}}, spec});
            EXPECT_FALSE(adjacency->data.empty());
            const auto all_edges = visitor->visit(GetAllEdgesQuery{});
            bool found = false;
            for (const auto &row: all_edges->data)
                if (row->fields.at(0).as<Edge>().global_id == edge->global_id)
                    found = true;
            EXPECT_TRUE(found);
        }
    }
} // namespace polygraph
