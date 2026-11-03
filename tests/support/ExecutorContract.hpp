#pragma once

#include "algorithms/bfs/BfsExecutor.hpp"
#include "infrastructure/BackendRegistry.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "support/ScriptedGraphStore.hpp"
#include "support/TestGraphStore.hpp"
#include "utilities/BackendIds.hpp"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>

namespace polygraph::test {
    using Json = nlohmann::json;
    inline std::vector<Json> oracle_cases() {
        std::ifstream file(POLYGRAPH_EXECUTOR_FIXTURES);
        if (!file)
            throw std::runtime_error("missing generated oracle fixtures");
        return Json::parse(file).get<std::vector<Json>>();
    }
    inline Json oracle_case(const std::string &name) {
        for (const auto &item: oracle_cases())
            if (item.at("name") == name)
                return item;
        throw std::runtime_error("unknown oracle case: " + name);
    }

    // Owns all dependencies for the real executor. Logical index -> encoded ID is a
    // bijection; expected hop counts and projected edges come only from Python.
    struct ExecutorGraph {
        std::shared_ptr<BackendRegistry> registry = std::make_shared<BackendRegistry>();
        PlacementCatalog catalog{registry};
        std::shared_ptr<ScriptedTopology> topology = std::make_shared<ScriptedTopology>();
        std::vector<std::shared_ptr<ScriptedGraphStore>> stores;
        std::map<GlobalId, std::size_t> logical;
        ExpandSpec spec;
        BfsOptions options;

        ExecutorGraph(const Json &fixture, bool split, ScriptedStoreConfig config = {}) {
            const std::vector<BackendId> routing{NEO4J, ARANGO};
            catalog.set_id_routing_backends(routing);
            std::vector<GlobalId> owned[2];
            const auto types = fixture.at("types").get<std::vector<TypeId>>();
            for (std::size_t i = 0; i < types.size(); ++i) {
                const auto partition = split ? i % 2 : 0;
                const auto id = insertion::BackendEncodedId::from_local_id(i + 1, routing[partition], routing);
                topology->vertices.push_back({id, types[i]});
                logical.emplace(id, i);
                owned[partition].push_back(id);
            }
            for (const auto &edge: fixture.at("edges"))
                topology->edges.push_back({topology->vertices.at(edge[0].get<std::size_t>()),
                                           topology->vertices.at(edge[1].get<std::size_t>()), edge[2].get<TypeId>()});
            for (std::size_t i = 0; i < (split ? 2U : 1U); ++i) {
                auto store =
                        std::make_shared<ScriptedGraphStore>(descriptor(routing[i], 10), topology, owned[i], config);
                registry->register_store(store);
                stores.push_back(std::move(store));
            }
            spec.direction = fixture.at("direction") == "both" ? Direction::Both : Direction::Directed;
            spec.allowed_edge_types = fixture.at("edge_types").get<std::vector<TypeId>>();
            spec.allowed_target_node_types = fixture.at("target_types").get<std::vector<TypeId>>();
            if (!fixture.at("max_depth").is_null())
                options.max_depth = fixture.at("max_depth").get<std::uint32_t>();
            options.allow_native_pushdown = false;
            options.streaming_candidate_batch_size = 1;
            options.scheduler_query_id = 123;
            options.pull_enter_frontier_percent = 40;
            options.pull_exit_frontier_percent = 20;
            options.bitmap_frontier_min_size = 2;
            options.bitmap_frontier_density_percent = 1;
        }
        std::shared_ptr<ScriptedGraphStore> add_pull(bool partial = false, bool stale = false,
                                                     ScriptedStoreConfig config = {}) {
            config.complete_pull = true;
            config.partial_pull = partial;
            std::vector<GlobalId> ids;
            for (const auto &vertex: topology->vertices)
                ids.push_back(vertex.global_id);
            auto store = std::make_shared<ScriptedGraphStore>(descriptor(MMAP, 1, BackendKind::MemoryMapped, !stale),
                                                              topology, ids, std::move(config));
            registry->register_store(store);
            // Deliberately keep pushes routed to their owners; this cache only provides pull.
            return store;
        }
        BfsResult run() { return BfsExecutor(catalog).run(topology->vertices.front(), spec, options); }
        BfsResult stream(std::vector<BfsLayerEvent> &layers) {
            const auto caller = std::this_thread::get_id();
            return BfsExecutor(catalog).run_streaming(topology->vertices.front(), spec, options,
                                                      [&](const BfsLayerEvent &event) {
                                                          EXPECT_EQ(std::this_thread::get_id(), caller);
                                                          for (const auto &store: stores)
                                                              EXPECT_EQ(store->active_calls(), 0U);
                                                          layers.push_back(event);
                                                      });
        }
        void check(const Json &fixture, const BfsResult &result) const {
            std::map<std::size_t, std::uint32_t> actual, expected;
            for (const auto &[id, distance]: result.distance) {
                ASSERT_TRUE(logical.contains(id)) << "retained invalid/transient ID " << id;
                actual.emplace(logical.at(id), distance);
            }
            for (const auto &[index, distance]: fixture.at("distance").items())
                expected.emplace(std::stoul(index), distance.get<std::uint32_t>());
            EXPECT_EQ(actual, expected);
            if (!options.return_predecessors) {
                EXPECT_TRUE(result.predecessor.empty());
                return;
            }
            EXPECT_EQ(result.predecessor.size(), expected.size() - 1);
            EXPECT_FALSE(result.predecessor.contains(topology->vertices.front().global_id));
            for (const auto &[child, parent]: result.predecessor) {
                ASSERT_TRUE(logical.contains(child));
                ASSERT_TRUE(logical.contains(parent));
                ASSERT_TRUE(result.distance.contains(parent));
                ASSERT_TRUE(result.distance.contains(child));
                EXPECT_EQ(result.distance.at(parent) + 1, result.distance.at(child));
                const auto neighbors =
                        fixture.at("adjacency").at(std::to_string(logical.at(parent))).get<std::vector<std::size_t>>();
                EXPECT_NE(std::find(neighbors.begin(), neighbors.end(), logical.at(child)), neighbors.end());
            }
        }
        void check_layers(const Json &fixture, const BfsResult &result,
                          const std::vector<BfsLayerEvent> &layers) const {
            std::map<std::uint32_t, std::set<GlobalId>> expected;
            for (const auto &[index, distance]: fixture.at("distance").items())
                expected[distance.get<std::uint32_t>()].insert(topology->vertices.at(std::stoul(index)).global_id);
            ASSERT_EQ(layers.size(), expected.size());
            std::set<GlobalId> all;
            for (std::size_t depth = 0; depth < layers.size(); ++depth) {
                const auto &layer = layers[depth];
                EXPECT_EQ(layer.depth, depth);
                std::set<GlobalId> actual;
                for (const auto &vertex: layer.vertices) {
                    EXPECT_TRUE(actual.insert(vertex.global_id).second);
                    EXPECT_TRUE(all.insert(vertex.global_id).second);
                    ASSERT_TRUE(logical.contains(vertex.global_id));
                    // An unresolved source is allowed at layer zero; discoveries must carry types.
                    if (depth > 0)
                        EXPECT_EQ(vertex.type, topology->vertices.at(logical.at(vertex.global_id)).type);
                }
                EXPECT_EQ(actual, expected.at(static_cast<std::uint32_t>(depth)));
                EXPECT_EQ(layer.tree_edges.size(), options.return_predecessors && depth > 0 ? actual.size() : 0U);
                std::set<GlobalId> targets;
                for (const auto &edge: layer.tree_edges) {
                    EXPECT_TRUE(targets.insert(edge.target.global_id).second);
                    EXPECT_TRUE(actual.contains(edge.target.global_id));
                    ASSERT_TRUE(result.predecessor.contains(edge.target.global_id));
                    EXPECT_EQ(edge.source.global_id, result.predecessor.at(edge.target.global_id));
                }
            }
        }
    };
} // namespace polygraph::test
