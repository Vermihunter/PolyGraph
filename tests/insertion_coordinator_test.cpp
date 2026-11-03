#include "insertion/BackendEncodedUuid.hpp"
#include "insertion/InsertionCoordinator.hpp"
#include "insertion/ReplicateEverywhereInsertionPlacementPolicy.hpp"
#include "insertion/TypeBasedInsertionPlacementPolicy.hpp"
#include "support/CapturingInsertStore.hpp"
#include "utilities/BackendIds.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <utility>
#include <vector>

namespace polygraph::insertion {
    namespace {

        struct CoordinatorFixture {
            std::shared_ptr<DatabaseContext> context = DatabaseContext::create_empty();
            std::shared_ptr<InMemoryBackendLoadTracker> tracker = std::make_shared<InMemoryBackendLoadTracker>();
            CoordinatorFixture() {
                context->id_routing_backends = {NEO4J, ARANGO, POSTGRESQL};
                context->placement_catalog->set_id_routing_backends(context->id_routing_backends);
            }

            TypeId person_type = context->schema->add_node_type("Person");
            TypeId company_type = context->schema->add_node_type("Company");
            TypeId knows_type = context->schema->add_edge_type("KNOWS", person_type, person_type);
            TypeId works_for_type = context->schema->add_edge_type("WORKS_FOR", person_type, company_type);

            std::shared_ptr<test::CapturingInsertStore> add_store(BackendId id, int priority = 100, bool fail = false) {
                auto store = std::make_shared<test::CapturingInsertStore>(
                        test::descriptor(id, priority, BackendKind::Neo4j), fail);
                context->register_store(store);
                return store;
            }

            static Node node(TypeId type, GlobalId id = invalid_global_id) {
                return Node{.global_id = id, .type = type, .labels = {"DomainNode"}, .properties = {}};
            }

            [[nodiscard]] Edge edge(TypeId type, GlobalId id = invalid_global_id) const {
                return Edge{.global_id = id,
                            .type = type,
                            .src = BackendEncodedId::from_local_id(1, NEO4J, context->id_routing_backends),
                            .dst = BackendEncodedId::from_local_id(2, ARANGO, context->id_routing_backends),
                            .properties = {}};
            }
        };

        class EmptyPlacementPolicy final : public IInsertionPlacementPolicy {
        public:
            PlacementDecision place_node(const Node &, PlacementContext &) const override { return {}; }
            PlacementDecision place_edge(const Edge &, PlacementContext &) const override { return {}; }
        };

        TEST(InsertionCoordinatorTest, RejectsNullPolicy) {
            CoordinatorFixture fixture;

            EXPECT_THROW(InsertionCoordinator(*fixture.context, nullptr, fixture.tracker), std::invalid_argument);
        }

        TEST(InsertionCoordinatorTest, RejectsNullLoadTracker) {
            CoordinatorFixture fixture;
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});

            EXPECT_THROW(InsertionCoordinator(*fixture.context, policy, nullptr), std::invalid_argument);
        }

        TEST(InsertionCoordinatorTest, EmptyBatchDoesNotTouchStores) {
            CoordinatorFixture fixture;
            auto store = fixture.add_store(3);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            EXPECT_TRUE(coordinator.insert_batch({}).empty());
            EXPECT_TRUE(store->batches().empty());
        }

        TEST(InsertionCoordinatorTest, RoutesNodeToConfiguredBackend) {
            CoordinatorFixture fixture;
            auto neo4j = fixture.add_store(3);
            auto arango = fixture.add_store(4);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            const InsertionReceipt receipt = coordinator.insert(CoordinatorFixture::node(fixture.person_type));

            EXPECT_EQ(receipt.target_backends, std::vector<BackendId>({3}));
            EXPECT_EQ(receipt.home_backend, 3U);
            EXPECT_EQ(neo4j->inserted_count(), 1U);
            EXPECT_EQ(arango->inserted_count(), 0U);
            ASSERT_EQ(neo4j->batches().front().size(), 1U);
            EXPECT_TRUE(neo4j->batches().front().front().is_node());
        }

        TEST(InsertionCoordinatorTest, RoutesEdgesUsingIndependentMap) {
            CoordinatorFixture fixture;
            auto neo4j = fixture.add_store(3);
            auto arango = fixture.add_store(4);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.knows_type, {4}}});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            const InsertionReceipt receipt = coordinator.insert(fixture.edge(fixture.knows_type));

            EXPECT_EQ(receipt.target_backends, std::vector<BackendId>({4}));
            EXPECT_EQ(neo4j->inserted_count(), 0U);
            EXPECT_EQ(arango->inserted_count(), 1U);
            EXPECT_TRUE(arango->batches().front().front().is_edge());
        }

        TEST(InsertionCoordinatorTest, GroupsManyEntitiesIntoOneBatchPerBackend) {
            CoordinatorFixture fixture;
            auto neo4j = fixture.add_store(3);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}},
                                                                {fixture.company_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.knows_type, {3}}});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);
            std::vector<OwnedInsertionEntity> entities;
            entities.emplace_back(CoordinatorFixture::node(fixture.person_type));
            entities.emplace_back(CoordinatorFixture::node(fixture.company_type));
            entities.emplace_back(fixture.edge(fixture.knows_type));

            const auto receipts = coordinator.insert_batch(std::move(entities));

            EXPECT_EQ(receipts.size(), 3U);
            ASSERT_EQ(neo4j->batches().size(), 1U);
            EXPECT_EQ(neo4j->batches().front().size(), 3U);
        }

        TEST(InsertionCoordinatorTest, SameEntityCanBeWrittenToMultipleBackends) {
            CoordinatorFixture fixture;
            auto neo4j = fixture.add_store(3);
            auto arango = fixture.add_store(4);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3, 4}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            const InsertionReceipt receipt = coordinator.insert(CoordinatorFixture::node(fixture.person_type));

            EXPECT_EQ(receipt.target_backends, std::vector<BackendId>({3, 4}));
            EXPECT_EQ(neo4j->inserted_count(), 1U);
            EXPECT_EQ(arango->inserted_count(), 1U);
            EXPECT_EQ(neo4j->batches().front().front().global_id(), arango->batches().front().front().global_id());
        }

        TEST(InsertionCoordinatorTest, GeneratesIdWhenInputIdIsInvalid) {
            CoordinatorFixture fixture;
            fixture.add_store(3);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            const InsertionReceipt receipt = coordinator.insert(CoordinatorFixture::node(fixture.person_type));

            EXPECT_NE(receipt.global_id, invalid_global_id);
            EXPECT_EQ(BackendEncodedId::decode(receipt.global_id, fixture.context->id_routing_backends), NEO4J);
        }

        TEST(InsertionCoordinatorTest, PreservesProvidedIdWhenEncodingIsDisabled) {
            CoordinatorFixture fixture;
            fixture.add_store(3);
            const GlobalId id = BackendEncodedId::from_local_id(42, ARANGO, fixture.context->id_routing_backends);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            EXPECT_EQ(coordinator.insert(CoordinatorFixture::node(fixture.person_type, id)).global_id, id);
        }

        TEST(InsertionCoordinatorTest, ZeroIsAProvidedIdRatherThanTheGenerationSentinel) {
            CoordinatorFixture fixture;
            auto store = fixture.add_store(NEO4J);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {NEO4J}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            const auto receipt = coordinator.insert(CoordinatorFixture::node(fixture.person_type, 0));

            EXPECT_EQ(receipt.global_id, 0U);
            ASSERT_EQ(store->inserted_count(), 1U);
            EXPECT_EQ(store->batches().front().front().global_id(), 0U);
        }

        TEST(InsertionCoordinatorTest, ExplicitLocalIdPreservesZeroAndEncodesTheSelectedOwner) {
            CoordinatorFixture fixture;
            auto store = fixture.add_store(POSTGRESQL);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {POSTGRESQL}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            const auto receipt = coordinator.insert(CoordinatorFixture::node(fixture.person_type), 0);

            EXPECT_EQ(receipt.global_id, 0x8000000000000000ULL);
            EXPECT_EQ(BackendEncodedId::decode(receipt.global_id, fixture.context->id_routing_backends), POSTGRESQL);
            ASSERT_EQ(store->inserted_count(), 1U);
            EXPECT_EQ(store->batches().front().front().global_id(), receipt.global_id);
        }

        TEST(InsertionCoordinatorTest, RejectsOversizedLocalIdBeforeWriting) {
            CoordinatorFixture fixture;
            auto store = fixture.add_store(NEO4J);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {NEO4J}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            EXPECT_THROW((void) coordinator.insert(CoordinatorFixture::node(fixture.person_type), GlobalId{1} << 48U),
                         std::overflow_error);
            EXPECT_EQ(store->inserted_count(), 0U);
            EXPECT_EQ(fixture.tracker->load(NEO4J).stored_nodes, 0U);
        }

        TEST(InsertionCoordinatorTest, EncodesHomeBackendInGeneratedId) {
            CoordinatorFixture fixture;
            fixture.add_store(3);
            fixture.add_store(4);
            auto selector = std::make_shared<LeastLoadedHomeBackendSelector>();
            auto policy = std::make_shared<ReplicateEverywhereInsertionPlacementPolicy>(selector,
                                                                                        std::vector<BackendId>{3, 4});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            const InsertionReceipt receipt = coordinator.insert(CoordinatorFixture::node(fixture.person_type));

            ASSERT_TRUE(receipt.home_backend.has_value());
            EXPECT_EQ(BackendEncodedId::decode(receipt.global_id, fixture.context->id_routing_backends),
                      *receipt.home_backend);
        }

        TEST(InsertionCoordinatorTest, ReencodingPreservesPayloadAndCacheResidency) {
            CoordinatorFixture fixture;
            fixture.add_store(3);
            const GlobalId original = BackendEncodedId::from_local_id(
                    0x123456789ABCULL, ARANGO, fixture.context->id_routing_backends, BackendEncodedId::MMAP_LOCATION);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{}, std::vector<BackendId>{}, std::vector<BackendId>{},
                    true);
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            const GlobalId encoded =
                    coordinator.insert(CoordinatorFixture::node(fixture.person_type, original)).global_id;

            EXPECT_EQ(BackendEncodedId::decode(encoded, fixture.context->id_routing_backends), NEO4J);
            EXPECT_EQ(BackendEncodedId::local_id(encoded, fixture.context->id_routing_backends), 0x123456789ABCULL);
            EXPECT_EQ(BackendEncodedId::cache_residency(encoded), BackendEncodedId::MMAP_LOCATION);
        }

        TEST(InsertionCoordinatorTest, ReplicatedBatchBalancesHomeOwnershipWithinBatch) {
            CoordinatorFixture fixture;
            fixture.add_store(3, 10);
            fixture.add_store(4, 10);
            auto selector = std::make_shared<LeastLoadedHomeBackendSelector>();
            auto policy = std::make_shared<ReplicateEverywhereInsertionPlacementPolicy>(selector,
                                                                                        std::vector<BackendId>{3, 4});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);
            std::vector<OwnedInsertionEntity> entities;
            entities.emplace_back(CoordinatorFixture::node(fixture.person_type));
            entities.emplace_back(CoordinatorFixture::node(fixture.person_type));
            entities.emplace_back(CoordinatorFixture::node(fixture.person_type));
            entities.emplace_back(CoordinatorFixture::node(fixture.person_type));

            const auto receipts = coordinator.insert_batch(std::move(entities));

            ASSERT_EQ(receipts.size(), 4U);
            EXPECT_EQ(receipts[0].home_backend, 3U);
            EXPECT_EQ(receipts[1].home_backend, 4U);
            EXPECT_EQ(receipts[2].home_backend, 3U);
            EXPECT_EQ(receipts[3].home_backend, 4U);
        }

        TEST(InsertionCoordinatorTest, TracksOwnedAndStoredCountsForReplicas) {
            CoordinatorFixture fixture;
            fixture.add_store(3, 10);
            fixture.add_store(4, 10);
            auto selector = std::make_shared<LeastLoadedHomeBackendSelector>();
            auto policy = std::make_shared<ReplicateEverywhereInsertionPlacementPolicy>(selector,
                                                                                        std::vector<BackendId>{3, 4});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);
            std::vector<OwnedInsertionEntity> entities;
            entities.emplace_back(CoordinatorFixture::node(fixture.person_type));
            entities.emplace_back(CoordinatorFixture::node(fixture.person_type));

            (void) coordinator.insert_batch(std::move(entities));

            const BackendLoad first = fixture.tracker->load(3);
            const BackendLoad second = fixture.tracker->load(4);
            EXPECT_EQ(first.stored_nodes, 2U);
            EXPECT_EQ(second.stored_nodes, 2U);
            EXPECT_EQ(first.owned_nodes, 1U);
            EXPECT_EQ(second.owned_nodes, 1U);
        }

        TEST(InsertionCoordinatorTest, TracksNodeAndEdgeLoadsSeparately) {
            CoordinatorFixture fixture;
            fixture.add_store(3);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.knows_type, {3}}});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);
            std::vector<OwnedInsertionEntity> entities;
            entities.emplace_back(CoordinatorFixture::node(fixture.person_type));
            entities.emplace_back(fixture.edge(fixture.knows_type));

            (void) coordinator.insert_batch(std::move(entities));

            const BackendLoad load = fixture.tracker->load(3);
            EXPECT_EQ(load.owned_nodes, 1U);
            EXPECT_EQ(load.stored_nodes, 1U);
            EXPECT_EQ(load.owned_edges, 1U);
            EXPECT_EQ(load.stored_edges, 1U);
        }

        TEST(InsertionCoordinatorTest, RejectsInvalidPolicyDecisionBeforeWriting) {
            CoordinatorFixture fixture;
            auto store = fixture.add_store(3);
            auto policy = std::make_shared<EmptyPlacementPolicy>();
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            EXPECT_THROW((void) coordinator.insert(CoordinatorFixture::node(fixture.person_type)), std::runtime_error);
            EXPECT_EQ(store->inserted_count(), 0U);
        }

        TEST(InsertionCoordinatorTest, WrapsBackendWriteFailureWithBackendId) {
            CoordinatorFixture fixture;
            fixture.add_store(3, 100, true);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            try {
                (void) coordinator.insert(CoordinatorFixture::node(fixture.person_type));
                FAIL() << "Expected insertion failure";
            } catch (const std::runtime_error &error) {
                EXPECT_NE(std::string(error.what()).find("backend 3"), std::string::npos);
            }
        }

        TEST(InsertionCoordinatorTest, DoesNotRecordLoadsWhenAnyBackendFails) {
            CoordinatorFixture fixture;
            auto first_store = fixture.add_store(3);
            fixture.add_store(4, 100, true);
            auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
                    TypeBasedInsertionPlacementPolicy::RouteMap{{fixture.person_type, {3, 4}}},
                    TypeBasedInsertionPlacementPolicy::RouteMap{});
            InsertionCoordinator coordinator(*fixture.context, policy, fixture.tracker);

            EXPECT_THROW((void) coordinator.insert(fixture.node(fixture.person_type)), std::runtime_error);
            EXPECT_EQ(first_store->inserted_count(), 1U);
            EXPECT_EQ(fixture.tracker->load(3).stored_nodes, 0U);
            EXPECT_EQ(fixture.tracker->load(4).stored_nodes, 0U);
        }

    } // namespace
} // namespace polygraph::insertion
