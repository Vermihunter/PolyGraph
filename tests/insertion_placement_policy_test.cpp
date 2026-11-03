#include "insertion/HomeBackendSelector.hpp"
#include "insertion/ReplicateEverywhereInsertionPlacementPolicy.hpp"
#include "insertion/TypeBasedInsertionPlacementPolicy.hpp"
#include "support/TestGraphStore.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace polygraph::insertion {
    namespace {

        Node node(TypeId type) { return Node{.global_id = GlobalId{42}, .type = type, .labels = {}, .properties = {}}; }

        Edge edge(TypeId type) {
            return Edge{
                    .global_id = GlobalId{73}, .type = type, .src = GlobalId{1}, .dst = GlobalId{2}, .properties = {}};
        }

        struct PolicyFixture {
            BackendRegistry registry;
            InMemoryBackendLoadTracker tracker;
            BackendLoadSnapshot snapshot{tracker};

            void add(BackendId id, int priority = 100) {
                registry.register_store(
                        std::make_shared<test::TestGraphStore>(test::descriptor(id, priority, BackendKind::Neo4j)));
            }

            PlacementContext context() {
                return PlacementContext{.backend_registry = registry, .load_snapshot = snapshot};
            }
        };

        TEST(TypeBasedInsertionPlacementPolicyTest, RoutesNodeByType) {
            TypeBasedInsertionPlacementPolicy policy({{10, {3}}}, {});
            PolicyFixture fixture;
            fixture.add(3);
            auto context = fixture.context();

            const PlacementDecision decision = policy.place_node(node(10), context);

            EXPECT_EQ(decision.target_backends, std::vector<BackendId>({3}));
            EXPECT_EQ(decision.home_backend, 3U);
            EXPECT_FALSE(decision.encode_home_backend_in_uuid);
        }

        TEST(TypeBasedInsertionPlacementPolicyTest, RoutesEdgeIndependentlyFromNodeRoutes) {
            TypeBasedInsertionPlacementPolicy policy({{10, {3}}}, {{20, {4}}});
            PolicyFixture fixture;
            fixture.add(4);
            auto context = fixture.context();

            const PlacementDecision decision = policy.place_edge(edge(20), context);

            EXPECT_EQ(decision.target_backends, std::vector<BackendId>({4}));
            EXPECT_EQ(decision.home_backend, 4U);
        }

        TEST(TypeBasedInsertionPlacementPolicyTest, SupportsReplicatedRouteForOneType) {
            TypeBasedInsertionPlacementPolicy policy({{10, {3, 4}}}, {});
            PolicyFixture fixture;
            fixture.add(3);
            fixture.add(4);
            auto context = fixture.context();

            const PlacementDecision decision = policy.place_node(node(10), context);

            EXPECT_EQ(decision.target_backends, std::vector<BackendId>({3, 4}));
            EXPECT_EQ(decision.home_backend, 3U);
        }

        TEST(TypeBasedInsertionPlacementPolicyTest, UsesNodeFallback) {
            TypeBasedInsertionPlacementPolicy policy({}, {}, {8}, {});
            PolicyFixture fixture;
            fixture.add(8);
            auto context = fixture.context();

            EXPECT_EQ(policy.place_node(node(999), context).target_backends, std::vector<BackendId>({8}));
        }

        TEST(TypeBasedInsertionPlacementPolicyTest, UsesEdgeFallback) {
            TypeBasedInsertionPlacementPolicy policy({}, {}, {}, {9});
            PolicyFixture fixture;
            fixture.add(9);
            auto context = fixture.context();

            EXPECT_EQ(policy.place_edge(edge(999), context).target_backends, std::vector<BackendId>({9}));
        }

        TEST(TypeBasedInsertionPlacementPolicyTest, RejectsUnmappedTypeWithoutFallback) {
            TypeBasedInsertionPlacementPolicy policy({}, {});
            PolicyFixture fixture;
            auto context = fixture.context();

            EXPECT_THROW((void) policy.place_node(node(1), context), std::runtime_error);
            EXPECT_THROW((void) policy.place_edge(edge(2), context), std::runtime_error);
        }

        TEST(TypeBasedInsertionPlacementPolicyTest, CanRequestIdOwnershipEncoding) {
            TypeBasedInsertionPlacementPolicy policy({{10, {3}}}, {}, {}, {}, true);
            PolicyFixture fixture;
            fixture.add(3);
            auto context = fixture.context();

            EXPECT_TRUE(policy.place_node(node(10), context).encode_home_backend_in_uuid);
        }

        TEST(PlacementDecisionValidationTest, AcceptsValidDecision) {
            PolicyFixture fixture;
            fixture.add(3);

            EXPECT_NO_THROW(validate_placement_decision(PlacementDecision{.target_backends = {3}, .home_backend = 3},
                                                        fixture.registry));
        }

        TEST(PlacementDecisionValidationTest, RejectsNoTargets) {
            PolicyFixture fixture;

            EXPECT_THROW(validate_placement_decision({}, fixture.registry), std::runtime_error);
        }

        TEST(PlacementDecisionValidationTest, RejectsUnknownTarget) {
            PolicyFixture fixture;

            EXPECT_THROW(validate_placement_decision(
                                 PlacementDecision{.target_backends = {404}, .home_backend = std::nullopt},
                                 fixture.registry),
                         std::runtime_error);
        }

        TEST(PlacementDecisionValidationTest, RejectsDuplicateTargets) {
            PolicyFixture fixture;
            fixture.add(3);

            EXPECT_THROW(validate_placement_decision(
                                 PlacementDecision{.target_backends = {3, 3}, .home_backend = std::nullopt},
                                 fixture.registry),
                         std::runtime_error);
        }

        TEST(PlacementDecisionValidationTest, RejectsHomeOutsideTargets) {
            PolicyFixture fixture;
            fixture.add(3);
            fixture.add(4);

            EXPECT_THROW(validate_placement_decision(PlacementDecision{.target_backends = {3}, .home_backend = 4},
                                                     fixture.registry),
                         std::runtime_error);
        }

        TEST(PlacementDecisionValidationTest, RejectsEncodingWithoutHome) {
            PolicyFixture fixture;
            fixture.add(3);

            EXPECT_THROW(validate_placement_decision(PlacementDecision{.target_backends = {3},
                                                                       .home_backend = std::nullopt,
                                                                       .encode_home_backend_in_uuid = true},
                                                     fixture.registry),
                         std::runtime_error);
        }

        TEST(LeastLoadedHomeBackendSelectorTest, ChoosesBackendWithFewestOwnedNodes) {
            PolicyFixture fixture;
            fixture.add(3);
            fixture.add(4);
            fixture.tracker.set(3, BackendLoad{.owned_nodes = 20});
            fixture.tracker.set(4, BackendLoad{.owned_nodes = 2});
            BackendLoadSnapshot snapshot(fixture.tracker);
            PlacementContext context{fixture.registry, snapshot};
            LeastLoadedHomeBackendSelector selector;

            EXPECT_EQ(selector.choose_for_node(node(10), {3, 4}, context), 4U);
        }

        TEST(LeastLoadedHomeBackendSelectorTest, ChoosesBackendWithFewestOwnedEdges) {
            PolicyFixture fixture;
            fixture.add(3);
            fixture.add(4);
            fixture.tracker.set(3, BackendLoad{.owned_edges = 1});
            fixture.tracker.set(4, BackendLoad{.owned_edges = 9});
            BackendLoadSnapshot snapshot(fixture.tracker);
            PlacementContext context{fixture.registry, snapshot};
            LeastLoadedHomeBackendSelector selector;

            EXPECT_EQ(selector.choose_for_edge(edge(20), {3, 4}, context), 3U);
        }

        TEST(LeastLoadedHomeBackendSelectorTest, UsesBackendPriorityAsTieBreaker) {
            PolicyFixture fixture;
            fixture.add(3, 50);
            fixture.add(4, 5);
            BackendLoadSnapshot snapshot(fixture.tracker);
            PlacementContext context{fixture.registry, snapshot};
            LeastLoadedHomeBackendSelector selector;

            EXPECT_EQ(selector.choose_for_node(node(10), {3, 4}, context), 4U);
        }

        TEST(LeastLoadedHomeBackendSelectorTest, UsesBackendIdWhenScoresAreIdentical) {
            PolicyFixture fixture;
            fixture.add(3, 5);
            fixture.add(4, 5);
            BackendLoadSnapshot snapshot(fixture.tracker);
            PlacementContext context{fixture.registry, snapshot};
            LeastLoadedHomeBackendSelector selector;

            EXPECT_EQ(selector.choose_for_node(node(10), {4, 3}, context), 3U);
        }

        TEST(LeastLoadedHomeBackendSelectorTest, RejectsEmptyCandidates) {
            PolicyFixture fixture;
            BackendLoadSnapshot snapshot(fixture.tracker);
            PlacementContext context{fixture.registry, snapshot};
            LeastLoadedHomeBackendSelector selector;

            EXPECT_THROW((void) selector.choose_for_node(node(10), {}, context), std::invalid_argument);
        }

        TEST(ReplicateEverywhereInsertionPlacementPolicyTest, UsesExplicitReplicaSet) {
            PolicyFixture fixture;
            fixture.add(3);
            fixture.add(4);
            auto selector = std::make_shared<LeastLoadedHomeBackendSelector>();
            ReplicateEverywhereInsertionPlacementPolicy policy(selector, {4, 3, 4});
            auto context = fixture.context();

            const PlacementDecision decision = policy.place_node(node(10), context);

            EXPECT_EQ(decision.target_backends, std::vector<BackendId>({3, 4}));
            EXPECT_TRUE(decision.home_backend.has_value());
            EXPECT_TRUE(decision.encode_home_backend_in_uuid);
        }

        TEST(ReplicateEverywhereInsertionPlacementPolicyTest, EmptyReplicaSetMeansAllRegisteredStores) {
            PolicyFixture fixture;
            fixture.add(8);
            fixture.add(2);
            auto selector = std::make_shared<LeastLoadedHomeBackendSelector>();
            ReplicateEverywhereInsertionPlacementPolicy policy(selector);
            auto context = fixture.context();

            EXPECT_EQ(policy.place_edge(edge(20), context).target_backends, std::vector<BackendId>({2, 8}));
        }

        TEST(ReplicateEverywhereInsertionPlacementPolicyTest, RejectsNullSelector) {
            EXPECT_THROW(ReplicateEverywhereInsertionPlacementPolicy(nullptr), std::invalid_argument);
        }

        TEST(ReplicateEverywhereInsertionPlacementPolicyTest, RejectsNoAvailableBackends) {
            PolicyFixture fixture;
            auto selector = std::make_shared<LeastLoadedHomeBackendSelector>();
            ReplicateEverywhereInsertionPlacementPolicy policy(selector);
            auto context = fixture.context();

            EXPECT_THROW((void) policy.place_node(node(1), context), std::runtime_error);
        }

    } // namespace
} // namespace polygraph::insertion
