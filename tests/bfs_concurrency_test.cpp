#include "support/ExecutorContract.hpp"

#include <atomic>
#include <future>

namespace polygraph::test {
    namespace {
        using namespace std::chrono_literals;
        class BfsConcurrencyTest : public testing::TestWithParam<int> {};

        // 0/1 Classic; 2/3 adaptive fallback; 4/5 real adaptive push with a
        // complete eligible pull store. Bit zero selects predecessor/streaming mode.
        void configure_concurrent_policy(ExecutorGraph &graph, int parameter) {
            graph.options.execution_policy =
                    parameter < 2 ? BfsExecutionPolicy::Classic : BfsExecutionPolicy::DirectionOptimized;
            if (parameter >= 4) {
                graph.add_pull();
                graph.options.pull_enter_frontier_percent = 100;
                graph.options.pull_exit_frontier_percent = 0;
                EXPECT_EQ(graph.catalog.choose_pull_backend(graph.spec, graph.options), MMAP);
            }
        }

        TEST_P(BfsConcurrencyTest, PartitionsOverlapAndEveryLevelWaitsForAllPartitions) {
            const auto fixture = oracle_case("diamond_both_all_unlimited");
            ScriptedGate entered, slow, fast_done;
            fast_done.release();
            std::atomic_bool slow_finished{false};
            std::atomic_bool premature{false};
            ScriptedStoreConfig config;
            config.before_expand = [&](const ExpansionCall &call) {
                if (call.depth == 1) {
                    entered.arrive_and_wait();
                    if (call.backend == ARANGO)
                        slow.arrive_and_wait();
                }
                if (call.depth >= 2 && !slow_finished.load())
                    premature = true;
            };
            config.after_expand = [&](const ExpansionCall &call) {
                if (call.depth == 1) {
                    if (call.backend == ARANGO)
                        slow_finished = true;
                    else
                        fast_done.arrive_and_wait();
                }
            };
            ExecutorGraph graph(fixture, true, config);
            graph.options.return_predecessors = (GetParam() & 1) != 0;
            configure_concurrent_policy(graph, GetParam());
            std::vector<BfsLayerEvent> layers;
            auto run = std::async(std::launch::async, [&] { return graph.stream(layers); });
            const bool concurrent = entered.wait_for_arrivals(2);
            entered.release();
            const bool slow_entered = slow.wait_for_arrivals(1);
            const bool fast_finished = fast_done.wait_for_arrivals(1);
            // One partition completed while the other is held. Neither the next
            // expansion nor its layer callback can legally run yet.
            const auto status_while_blocked = run.wait_for(0ms);
            const bool saw_premature = premature.load();
            slow.release(); // Always unblock workers before any fatal assertion.
            ASSERT_TRUE(concurrent);
            ASSERT_TRUE(slow_entered);
            ASSERT_TRUE(fast_finished);
            EXPECT_EQ(status_while_blocked, std::future_status::timeout);
            EXPECT_FALSE(saw_premature);
            ASSERT_EQ(run.wait_for(5s), std::future_status::ready);
            const auto result = run.get();
            EXPECT_FALSE(premature.load());
            graph.check(fixture, result);
            graph.check_layers(fixture, result, layers);
        }

        TEST_P(BfsConcurrencyTest, ConcurrentDuplicateDiscoveriesAndReusedBuffersAreSafe) {
            const auto fixture = oracle_case("duplicates_both_all_unlimited");
            ScriptedGate callbacks;
            ScriptedStoreConfig config;
            config.candidate_workers = 4;
            config.before_candidate_batch = [&](const ExpansionCall &call, std::size_t ordinal) {
                if (call.depth == 0 && ordinal < 4)
                    callbacks.arrive_and_wait();
            };
            ExecutorGraph graph(fixture, true, config);
            graph.options.return_predecessors = false;
            configure_concurrent_policy(graph, GetParam());
            std::vector<BfsLayerEvent> layers;
            auto run = std::async(std::launch::async,
                                  [&] { return (GetParam() & 1) ? graph.stream(layers) : graph.run(); });
            const bool overlapped = callbacks.wait_for_arrivals(4);
            callbacks.release();
            ASSERT_TRUE(overlapped);
            ASSERT_EQ(run.wait_for(5s), std::future_status::ready);
            const auto result = run.get();
            graph.check(fixture, result);
            if (GetParam() & 1)
                graph.check_layers(fixture, result, layers);
            // Buffer contents were overwritten immediately after every callback. The
            // next frontier still routed all discovered vertices to the right owner.
            for (const auto &store: graph.stores) {
                EXPECT_EQ(store->active_calls(), 0U);
                EXPECT_EQ(store->completed_calls(), store->calls().size());
            }
        }

        TEST_P(BfsConcurrencyTest, BackendExceptionJoinsBlockedPeerAndPropagatesWithoutNextLayer) {
            const auto fixture = oracle_case("diamond_both_all_unlimited");
            ScriptedGate peer, failed;
            failed.release();
            std::atomic_bool inject{true};
            ScriptedStoreConfig config;
            config.before_expand = [&](const ExpansionCall &call) {
                if (call.depth != 1 || !inject.load())
                    return;
                if (call.backend == ARANGO)
                    peer.arrive_and_wait();
                else {
                    failed.arrive_and_wait();
                    throw std::runtime_error("partition failed");
                }
            };
            ExecutorGraph graph(fixture, true, config);
            graph.options.return_predecessors = (GetParam() & 1) != 0;
            configure_concurrent_policy(graph, GetParam());
            std::vector<BfsLayerEvent> layers;
            auto run = std::async(std::launch::async, [&] { return graph.stream(layers); });
            const bool peer_entered = peer.wait_for_arrivals(1);
            const bool exception_entered = failed.wait_for_arrivals(1);
            const auto blocked = run.wait_for(0ms);
            peer.release();
            ASSERT_TRUE(peer_entered);
            ASSERT_TRUE(exception_entered);
            EXPECT_EQ(blocked, std::future_status::timeout);
            ASSERT_EQ(run.wait_for(5s), std::future_status::ready);
            EXPECT_THROW((void) run.get(), std::runtime_error);
            ASSERT_EQ(layers.size(), 2U);
            for (const auto &store: graph.stores) {
                EXPECT_EQ(store->active_calls(), 0U);
                for (const auto &call: store->calls())
                    EXPECT_LE(call.depth, 1);
            }
            inject = false;
            // A failed query must not leak visited or worker state into a subsequent run.
            graph.check(fixture, graph.run());
        }
        INSTANTIATE_TEST_SUITE_P(Paths, BfsConcurrencyTest, testing::Values(0, 1, 2, 3, 4, 5));

        TEST(BfsFailureContractTest, PartialCandidateFailureDoesNotEmitAnIncompleteLayer) {
            const auto fixture = oracle_case("diamond_both_all_unlimited");
            ExecutorGraph graph(fixture, true, {.candidate_workers = 1, .fail_after_batches = 1});
            graph.options.return_predecessors = false;
            std::vector<BfsLayerEvent> layers;
            EXPECT_THROW((void) graph.stream(layers), std::runtime_error);
            ASSERT_EQ(layers.size(), 1U);
            EXPECT_EQ(layers.front().depth, 0U);
            for (const auto &store: graph.stores)
                EXPECT_EQ(store->active_calls(), 0U);
        }

        TEST(BfsFailureContractTest, CandidateWorkerHookExceptionIsRethrownAfterWorkersJoin) {
            const auto fixture = oracle_case("duplicates_both_all_unlimited");
            ExecutorGraph graph(
                    fixture, false,
                    {.candidate_workers = 4, .after_candidate_batch = [](const ExpansionCall &, std::size_t) {
                         throw std::runtime_error("worker failed");
                     }});
            graph.options.return_predecessors = false;
            EXPECT_THROW((void) graph.run(), std::runtime_error);
            EXPECT_EQ(graph.stores.front()->active_calls(), 0U);
        }

        TEST(BfsFailureContractTest, PullExceptionPropagatesWithoutPublishingIncompleteLayer) {
            const auto fixture = oracle_case("switching_both_all_unlimited");
            for (const bool predecessors: {false, true}) {
                ExecutorGraph graph(fixture, true);
                auto pull = graph.add_pull(false, false, {.fail_before_depth = 1});
                graph.options.return_predecessors = predecessors;
                graph.options.execution_policy = BfsExecutionPolicy::DirectionOptimized;
                std::vector<BfsLayerEvent> layers;
                EXPECT_THROW((void) graph.stream(layers), std::runtime_error);
                ASSERT_EQ(layers.size(), 2U);
                EXPECT_EQ(pull->active_calls(), 0U);
                ASSERT_EQ(pull->calls().size(), 1U);
                EXPECT_EQ(pull->calls().front().depth, 1);
                for (const auto &store: graph.stores)
                    for (const auto &call: store->calls())
                        EXPECT_EQ(call.depth, 0);
            }
        }

        TEST(BfsFailureContractTest, StreamingConsumerExceptionPropagatesAndStopsExpansion) {
            const auto fixture = oracle_case("chain_outgoing_all_unlimited");
            for (const bool predecessors: {false, true}) {
                ExecutorGraph graph(fixture, true);
                graph.options.return_predecessors = predecessors;
                EXPECT_THROW((void) BfsExecutor(graph.catalog)
                                     .run_streaming(graph.topology->vertices.front(), graph.spec, graph.options,
                                                    [](const BfsLayerEvent &layer) {
                                                        if (layer.depth == 1)
                                                            throw std::runtime_error("consumer failed");
                                                    }),
                             std::runtime_error);
                for (const auto &store: graph.stores) {
                    EXPECT_EQ(store->active_calls(), 0U);
                    for (const auto &call: store->calls())
                        EXPECT_EQ(call.depth, 0);
                }
            }
        }
    } // namespace
} // namespace polygraph::test
