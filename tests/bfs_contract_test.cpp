#include "support/ExecutorContract.hpp"

namespace polygraph::test {
    namespace {
        class BfsContractTest : public testing::TestWithParam<Json> {};

        TEST_P(BfsContractTest, MatchesIndependentOracleAcrossExecutionModes) {
            const auto &fixture = GetParam();
            for (const bool split: {false, true}) {
                for (const bool predecessors: {false, true}) {
                    // Classic; adaptive with absent/partial/stale pull; adaptive sparse/bitmap pull.
                    for (int mode = 0; mode < 6; ++mode) {
                        SCOPED_TRACE(fixture.at("name").get<std::string>() + " split=" + std::to_string(split) +
                                     " predecessors=" + std::to_string(predecessors) + " mode=" + std::to_string(mode));
                        ExecutorGraph graph(fixture, split);
                        graph.options.return_predecessors = predecessors;
                        graph.options.execution_policy =
                                mode == 0 ? BfsExecutionPolicy::Classic : BfsExecutionPolicy::DirectionOptimized;
                        std::shared_ptr<ScriptedGraphStore> pull;
                        if (mode >= 2)
                            pull = graph.add_pull(mode == 2, mode == 3);
                        if (mode == 4)
                            graph.options.bitmap_frontier_min_size = 1000;
                        const auto materialized = graph.run();
                        graph.check(fixture, materialized);
                        std::vector<BfsLayerEvent> layers;
                        const auto streamed = graph.stream(layers);
                        graph.check(fixture, streamed);
                        graph.check_layers(fixture, streamed, layers);
                        EXPECT_EQ(materialized.distance, streamed.distance);
                        if (pull && (mode < 4 || graph.spec.direction == Direction::Directed))
                            EXPECT_TRUE(pull->calls().empty());
                        for (const auto &store: graph.stores) {
                            if (graph.options.max_depth == 0)
                                EXPECT_TRUE(store->calls().empty());
                            for (const auto &call: store->calls()) {
                                EXPECT_EQ(call.query_id, 123U);
                                for (const auto &source: call.frontier) {
                                    EXPECT_EQ(graph.catalog.owner_backend(source.global_id), call.backend);
                                    ASSERT_TRUE(materialized.distance.contains(source.global_id));
                                    EXPECT_EQ(materialized.distance.at(source.global_id), call.depth);
                                }
                                EXPECT_EQ(call.kind, predecessors ? ExpansionKind::Rows : ExpansionKind::Candidates);
                            }
                        }
                    }
                }
            }
        }
        INSTANTIATE_TEST_SUITE_P(Oracle, BfsContractTest, testing::ValuesIn(oracle_cases()),
                                 [](const testing::TestParamInfo<Json> &info) {
                                     return info.param.at("name").get<std::string>();
                                 });

        TEST(BfsPolicyContractTest, ProvesPushPullPushForSparseAndBitmapMembership) {
            const auto fixture = oracle_case("switching_both_all_unlimited");
            for (const bool predecessors: {false, true}) {
                for (const bool bitmap: {false, true}) {
                    ExecutorGraph graph(fixture, true);
                    auto pull = graph.add_pull();
                    graph.options.return_predecessors = predecessors;
                    graph.options.execution_policy = BfsExecutionPolicy::DirectionOptimized;
                    graph.options.bitmap_frontier_min_size = bitmap ? 2 : 1000;
                    std::vector<BfsLayerEvent> layers;
                    const auto result = graph.stream(layers);
                    graph.check(fixture, result);
                    graph.check_layers(fixture, result, layers);
                    ASSERT_EQ(pull->calls().size(), 1U);
                    EXPECT_EQ(pull->calls().front().kind, ExpansionKind::Pull);
                    EXPECT_EQ(pull->calls().front().depth, 1);
                    std::set<int> push_depths;
                    for (const auto &store: graph.stores)
                        for (const auto &call: store->calls())
                            push_depths.insert(call.depth);
                    EXPECT_EQ(push_depths, (std::set<int>{0, 2, 3, 4}));
                }
            }
        }

        TEST(BfsPolicyContractTest, ForcedPullHonorsRelationshipAndDestinationFilters) {
            for (const auto &name: {"filters_both_relation_unlimited", "filters_both_target_unlimited",
                                    "filters_both_combined_unlimited"}) {
                const auto fixture = oracle_case(name);
                for (const bool predecessors: {false, true}) {
                    for (const bool bitmap: {false, true}) {
                        SCOPED_TRACE(name);
                        ExecutorGraph graph(fixture, true);
                        auto pull = graph.add_pull();
                        graph.options.return_predecessors = predecessors;
                        graph.options.execution_policy = BfsExecutionPolicy::DirectionOptimized;
                        graph.options.pull_enter_frontier_percent = 1;
                        graph.options.pull_exit_frontier_percent = 0;
                        graph.options.bitmap_frontier_min_size = bitmap ? 1 : 1000;
                        std::vector<BfsLayerEvent> layers;
                        const auto result = graph.stream(layers);
                        graph.check(fixture, result);
                        graph.check_layers(fixture, result, layers);
                        ASSERT_FALSE(pull->calls().empty());
                        for (const auto &call: pull->calls())
                            EXPECT_EQ(call.kind, ExpansionKind::Pull);
                        for (const auto &store: graph.stores)
                            EXPECT_TRUE(store->calls().empty());
                    }
                }
            }
        }

        TEST(BfsPolicyContractTest, ClassicNeverPullsEvenWithCompleteCapableStore) {
            const auto fixture = oracle_case("switching_both_all_unlimited");
            ExecutorGraph graph(fixture, true);
            auto pull = graph.add_pull();
            graph.check(fixture, graph.run());
            EXPECT_TRUE(pull->calls().empty());
        }

        TEST(BfsPolicyContractTest, ResolvesUnknownSourceTypeBeforeFilteredExpansion) {
            const auto fixture = oracle_case("filters_both_target_unlimited");
            for (const bool predecessors: {false, true}) {
                ExecutorGraph graph(fixture, true);
                graph.options.return_predecessors = predecessors;
                const auto source = NodeRef{graph.topology->vertices.front().global_id, unresolved_type_id};
                graph.check(fixture, BfsExecutor(graph.catalog).run(source, graph.spec, graph.options));
            }
        }
    } // namespace
} // namespace polygraph::test
