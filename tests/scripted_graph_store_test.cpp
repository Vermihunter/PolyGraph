#include "support/ScriptedGraphStore.hpp"
#include "support/TestGraphStore.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

namespace polygraph::test {
    namespace {

        std::shared_ptr<const ScriptedTopology> sample_topology() {
            return std::make_shared<const ScriptedTopology>(ScriptedTopology{
                    .vertices = {{1, 1}, {2, 2}, {3, 1}, {4, 2}},
                    .edges = {{{1, 1}, {2, 2}, 7}, {{3, 1}, {1, 1}, 8}, {{2, 2}, {4, 2}, 7}},
            });
        }

        TEST(ScriptedGraphStoreTest, ExpandsOnlyOneHopWithDirectionAndBothTypeFilters) {
            ScriptedGraphStore store(descriptor(1), sample_topology(), {1, 2, 3, 4});
            const std::vector<NodeRef> frontier{{1, unresolved_type_id}};
            const ExpandSpec directed{
                    .allowed_edge_types = {7}, .allowed_target_node_types = {2}, .direction = Direction::Directed};
            const auto rows = store.get_minimal_neighbors_batch(frontier, directed, 0, {});
            ASSERT_EQ(rows.size(), 1U);
            EXPECT_EQ(rows.front().source.type, 1U);
            ASSERT_EQ(rows.front().neighbors.size(), 1U);
            EXPECT_EQ(rows.front().neighbors.front().global_id, 2U);
            EXPECT_EQ(rows.front().neighbors.front().type, 2U);

            const ExpandSpec incoming{
                    .allowed_edge_types = {8}, .allowed_target_node_types = {1}, .direction = Direction::Both};
            const auto reverse = store.get_neighbors_batch(frontier, incoming, 0);
            ASSERT_EQ(reverse.size(), 1U);
            ASSERT_EQ(reverse.front().neighbors.size(), 1U);
            EXPECT_EQ(reverse.front().neighbors.front().vertex.global_id, 3U);
        }

        TEST(ScriptedGraphStoreTest, RejectsNonresidentAndStaleSourcesAndNativeBfs) {
            ScriptedGraphStore store(descriptor(1), sample_topology(), {1});
            EXPECT_TRUE(store.can_expand({1, unresolved_type_id}, {}));
            EXPECT_FALSE(store.can_expand({2, 2}, {}));
            EXPECT_FALSE(store.can_run_native_bfs({1, 1}, {}, {}));
            ScriptedGraphStore stale(descriptor(1, 1, BackendKind::RamCache, false), sample_topology(), {1});
            EXPECT_FALSE(stale.can_expand({1, 1}, {}));
        }

        TEST(ScriptedGraphStoreTest, StreamsBoundedCandidateCopiesWithStableCallMetadata) {
            const auto topology = std::make_shared<const ScriptedTopology>(ScriptedTopology{
                    .vertices = {{1, 1}, {2, 2}, {3, 1}, {4, 2}},
                    .edges = {{{1, 1}, {2, 2}, 7}, {{1, 1}, {3, 1}, 8}, {{1, 1}, {4, 2}, 7}},
            });
            ScriptedGraphStore store(descriptor(1), topology, {1}, {.candidate_workers = 3});
            BfsOptions options;
            options.streaming_candidate_batch_size = 1;
            options.scheduler_query_id = 91;
            std::mutex copied_mutex;
            std::vector<GlobalId> copied;
            store.stream_neighbor_candidates(std::vector<NodeRef>{{1, 1}}, {}, 4, options,
                                             [&](std::span<const NodeRef> batch) {
                                                 ASSERT_LE(batch.size(), 1U);
                                                 std::scoped_lock lock(copied_mutex);
                                                 for (const auto &vertex: batch)
                                                     copied.push_back(vertex.global_id);
                                             });
            std::sort(copied.begin(), copied.end());
            EXPECT_EQ(copied, (std::vector<GlobalId>{2, 3, 4}));
            const auto calls = store.calls();
            ASSERT_EQ(calls.size(), 1U);
            EXPECT_EQ(calls.front().query_id, 91U);
            EXPECT_EQ(calls.front().depth, 4);
            EXPECT_EQ(calls.front().kind, ExpansionKind::Candidates);
            EXPECT_EQ(store.active_calls(), 0U);
            EXPECT_EQ(store.completed_calls(), 1U);
        }

        TEST(ScriptedGraphStoreTest, PullRequiresCompleteUniverseAndReturnsProjectedPredecessor) {
            const ExpandSpec spec{
                    .allowed_edge_types = {7}, .allowed_target_node_types = {2}, .direction = Direction::Both};
            ScriptedGraphStore complete(descriptor(1), sample_topology(), {1, 2, 3, 4}, {.complete_pull = true});
            EXPECT_TRUE(complete.can_pull_expand(spec));
            EXPECT_FALSE(complete.can_pull_expand(ExpandSpec{.direction = Direction::Directed}));
            ScriptedGraphStore partial(descriptor(1), sample_topology(), {1, 2},
                                       {.complete_pull = true, .partial_pull = true});
            EXPECT_FALSE(partial.can_pull_expand(spec));
            std::vector<PullCandidate> discoveries;
            BfsOptions options;
            options.streaming_candidate_batch_size = 1;
            complete.stream_pull_candidates(
                    spec, 0, options, [](GlobalId id) { return id == 1; }, [](GlobalId id) { return id == 1; },
                    [&](std::span<const PullCandidate> batch) {
                        EXPECT_LE(batch.size(), 1U);
                        discoveries.insert(discoveries.end(), batch.begin(), batch.end());
                    });
            ASSERT_EQ(discoveries.size(), 1U);
            EXPECT_EQ(discoveries.front().vertex.global_id, 2U);
            EXPECT_EQ(discoveries.front().predecessor.global_id, 1U);
        }

        TEST(ScriptedGraphStoreTest, DeliversConfiguredPartialBatchBeforeFailureAndCleansActiveState) {
            ScriptedGraphStore store(descriptor(1), sample_topology(), {1, 2, 3, 4}, {.fail_after_batches = 1});
            BfsOptions options;
            options.streaming_candidate_batch_size = 1;
            std::size_t delivered = 0;
            EXPECT_THROW(store.stream_neighbor_candidates(std::vector<NodeRef>{{1, 1}}, {}, 0, options,
                                                          [&](std::span<const NodeRef>) { ++delivered; }),
                         std::runtime_error);
            EXPECT_EQ(delivered, 1U);
            EXPECT_EQ(store.active_calls(), 0U);
            EXPECT_EQ(store.completed_calls(), 1U);
        }

        TEST(ScriptedGraphStoreTest, FailureBeforeDeliveryAlsoAppliesToEmptyParallelCandidates) {
            ScriptedGraphStore store(descriptor(1), sample_topology(), {1, 2, 3, 4},
                                     {.candidate_workers = 4, .fail_after_batches = 0});
            EXPECT_THROW(store.stream_neighbor_candidates(std::vector<NodeRef>{{4, 2}},
                                                          ExpandSpec{.direction = Direction::Directed}, 0, {},
                                                          [](std::span<const NodeRef>) {}),
                         std::runtime_error);
            EXPECT_EQ(store.active_calls(), 0U);
            EXPECT_EQ(store.completed_calls(), 1U);
        }

        TEST(ScriptedGraphStoreTest, ExceptionsFromHooksCleanCallState) {
            ScriptedGraphStore store(
                    descriptor(1), sample_topology(), {1},
                    {
                            .before_expand = [](const ExpansionCall &) { throw std::runtime_error("scripted hook"); },
                    });
            EXPECT_THROW(store.get_neighbors_batch(std::vector<NodeRef>{{1, 1}}, {}, 0), std::runtime_error);
            EXPECT_EQ(store.active_calls(), 0U);
            EXPECT_EQ(store.completed_calls(), 1U);
        }

        TEST(ScriptedGraphStoreTest, ConcurrentCallsShareOnlySynchronizedBookkeeping) {
            ScriptedGate entered;
            ScriptedGraphStore store(descriptor(1), sample_topology(), {1, 2, 3, 4},
                                     {.before_expand = [&](const ExpansionCall &) { entered.arrive_and_wait(); }});
            std::vector<std::future<std::vector<MinimalAdjacencyRow>>> runs;
            runs.reserve(4);
            for (int i = 0; i < 4; ++i) {
                runs.push_back(std::async(std::launch::async, [&] {
                    return store.get_minimal_neighbors_batch(std::vector<NodeRef>{{.global_id = 1, .type = 1}}, {}, 0,
                                                             {});
                }));
            }
            const bool overlap = entered.wait_for_arrivals(4);
            const auto active = store.active_calls();
            entered.release();
            ASSERT_TRUE(overlap);
            EXPECT_EQ(active, 4U);
            for (auto &run: runs) {
                ASSERT_EQ(run.wait_for(std::chrono::seconds(5)), std::future_status::ready);
                const auto rows = run.get();
                ASSERT_EQ(rows.size(), 1U);
                EXPECT_EQ(rows.front().neighbors.size(), 2U);
            }
            EXPECT_EQ(store.calls().size(), 4U);
            EXPECT_EQ(store.completed_calls(), 4U);
            EXPECT_EQ(store.active_calls(), 0U);
        }

        TEST(ScriptedGraphStoreTest, GateCoordinatesArrivalWithoutSleeping) {
            ScriptedGate gate;
            auto waiter = std::async(std::launch::async, [&] { gate.arrive_and_wait(); });
            const bool arrived = gate.wait_for_arrivals(1);
            gate.release();
            ASSERT_TRUE(arrived);
            ASSERT_EQ(waiter.wait_for(std::chrono::seconds(5)), std::future_status::ready);
            EXPECT_NO_THROW(waiter.get());
            EXPECT_EQ(gate.arrivals(), 1U);
        }

    } // namespace
} // namespace polygraph::test
