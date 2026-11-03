#include "infrastructure/PlacementCatalog.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "support/TestGraphStore.hpp"
#include "utilities/BackendIds.hpp"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

namespace polygraph {
    namespace {

        constexpr TypeId node_type = 1;
        constexpr BackendId primary_backend = NEO4J;
        constexpr BackendId cache_backend = REDIS;
        constexpr std::array routing_backends{primary_backend};

        const NodeRef source{
                insertion::BackendEncodedId::from_local_id(20, primary_backend, routing_backends),
                node_type,
        };

        struct CacheResidencyCase {
            bool cache_resident;
            bool cache_fresh;
            BackendId expected_backend;
        };

        class PlacementResidencyTest : public testing::TestWithParam<CacheResidencyCase> {
        protected:
            std::shared_ptr<BackendRegistry> registry = std::make_shared<BackendRegistry>();
            PlacementCatalog catalog{registry};

            void SetUp() override {
                registry->register_store(std::make_shared<test::TestGraphStore>(
                        test::descriptor(primary_backend, 50, BackendKind::Neo4j)));
                registry->register_store(std::make_shared<test::TestGraphStore>(
                        test::descriptor(cache_backend, 1, BackendKind::RedisCache, GetParam().cache_fresh)));
                catalog.set_id_routing_backends({primary_backend});
                const std::uint64_t residency =
                        GetParam().cache_resident ? insertion::BackendEncodedId::REDIS_LOCATION : 0U;
                catalog.set_cache_residency(std::make_shared<const std::vector<CacheResidencyRecord>>(
                        std::initializer_list<CacheResidencyRecord>{{source.global_id, residency}}));
            }
        };

        TEST_P(PlacementResidencyTest, ChoosesFreshResidentBackendWithLowestPriority) {
            const auto &test_case = GetParam();
            const std::vector<NodeRef> frontier{source};

            const auto plan = catalog.plan_frontier_expansion(frontier, ExpandSpec{},
                                                              BfsOptions{
                                                                      .max_depth = std::nullopt,
                                                                      .return_predecessors = true,
                                                                      .allow_native_pushdown = true,
                                                                      .allow_partial_acceleration = true,
                                                              });

            ASSERT_EQ(plan.backend_frontiers.size(), 1U);
            ASSERT_TRUE(plan.backend_frontiers.contains(test_case.expected_backend));
            const auto &selected_frontier = plan.backend_frontiers.at(test_case.expected_backend);
            ASSERT_EQ(selected_frontier.size(), 1U);
            EXPECT_EQ(selected_frontier.front().global_id, source.global_id);
            EXPECT_EQ(selected_frontier.front().type, source.type);
        }

        INSTANTIATE_TEST_SUITE_P(CacheResidency, PlacementResidencyTest,
                                 testing::Values(CacheResidencyCase{true, true, cache_backend},
                                                 CacheResidencyCase{false, true, primary_backend},
                                                 CacheResidencyCase{true, false, primary_backend}));

        TEST(PlacementCatalogTest, ChoosesLowestPriorityEligibleNativeBfsBackend) {
            auto registry = std::make_shared<BackendRegistry>();
            auto slower = std::make_shared<test::TestGraphStore>(test::descriptor(NEO4J, 30, BackendKind::Neo4j));
            auto faster = std::make_shared<test::TestGraphStore>(test::descriptor(ARANGO, 5, BackendKind::Arango));
            auto partial_descriptor = test::descriptor(MEMGRAPH, 0, BackendKind::Memgraph);
            partial_descriptor.coverage.complete_for_requested_subgraph = false;
            auto partial = std::make_shared<test::TestGraphStore>(partial_descriptor);
            slower->enable_native_bfs();
            faster->enable_native_bfs();
            partial->enable_native_bfs();
            registry->register_store(slower);
            registry->register_store(faster);
            registry->register_store(partial);
            PlacementCatalog catalog(registry);
            catalog.set_id_routing_backends({NEO4J, ARANGO, MEMGRAPH});

            const auto selected = catalog.choose_native_bfs_backend(source, ExpandSpec{}, BfsOptions{});

            ASSERT_TRUE(selected.has_value());
            EXPECT_EQ(*selected, ARANGO);
            EXPECT_FALSE(catalog.choose_native_bfs_backend(source, ExpandSpec{},
                                                           BfsOptions{.allow_native_pushdown = false}));
        }

        TEST(PlacementCatalogTest, RejectsVertexWithoutFreshResidentBackend) {
            auto registry = std::make_shared<BackendRegistry>();
            registry->register_store(std::make_shared<test::TestGraphStore>(
                    test::descriptor(primary_backend, 1, BackendKind::Neo4j, false)));
            PlacementCatalog catalog(registry);
            catalog.set_id_routing_backends({primary_backend});
            const std::vector<NodeRef> frontier{source};

            EXPECT_THROW((void) catalog.plan_frontier_expansion(frontier, ExpandSpec{}, BfsOptions{}),
                         std::runtime_error);
        }

        TEST(PlacementCatalogTest, RejectsExpansionWithoutRoutingLayout) {
            auto registry = std::make_shared<BackendRegistry>();
            registry->register_store(
                    std::make_shared<test::TestGraphStore>(test::descriptor(primary_backend, 1, BackendKind::Neo4j)));
            PlacementCatalog catalog(registry);
            const std::vector<NodeRef> frontier{source};

            EXPECT_THROW((void) catalog.plan_frontier_expansion(frontier, ExpandSpec{}, BfsOptions{}),
                         std::runtime_error);
        }

        TEST(PlacementCatalogTest, RejectsNullRegistry) {
            EXPECT_THROW(PlacementCatalog(nullptr), std::invalid_argument);
        }

    } // namespace
} // namespace polygraph
