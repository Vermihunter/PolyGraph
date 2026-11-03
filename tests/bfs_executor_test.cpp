#include "algorithms/bfs/BfsExecutor.hpp"
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

        constexpr BackendId backend_id = NEO4J;
        constexpr TypeId node_type = 1;
        constexpr std::array routing_backends{backend_id};

        const NodeRef a{insertion::BackendEncodedId::from_local_id(1, backend_id, routing_backends), node_type};
        const NodeRef b{insertion::BackendEncodedId::from_local_id(2, backend_id, routing_backends), node_type};
        const NodeRef c{insertion::BackendEncodedId::from_local_id(3, backend_id, routing_backends), node_type};
        const NodeRef d{insertion::BackendEncodedId::from_local_id(4, backend_id, routing_backends), node_type};
        const NodeRef e{insertion::BackendEncodedId::from_local_id(5, backend_id, routing_backends), node_type};

        struct BfsDepthCase {
            std::optional<std::uint32_t> max_depth;
            std::size_t expected_vertices;
            bool expects_d;
        };

        class BfsExecutorDepthTest : public testing::TestWithParam<BfsDepthCase> {
        protected:
            std::shared_ptr<BackendRegistry> registry = std::make_shared<BackendRegistry>();
            std::shared_ptr<test::TestGraphStore> store = std::make_shared<test::TestGraphStore>(
                    test::descriptor(backend_id, 10, BackendKind::Neo4j), test::TestGraphStore::Graph{
                                                                                  {a.global_id, {b, c}},
                                                                                  {b.global_id, {d}},
                                                                                  {c.global_id, {d}},
                                                                          });
            PlacementCatalog catalog{registry};

            void SetUp() override {
                registry->register_store(store);
                catalog.set_id_routing_backends({backend_id});
            }
        };

        TEST_P(BfsExecutorDepthTest, RespectsMaximumDepthAndDeduplicatesVertices) {
            const auto &test_case = GetParam();
            BfsExecutor executor(catalog);

            const auto result = executor.run(a,
                                             ExpandSpec{
                                                     .allowed_edge_types = {},
                                                     .allowed_target_node_types = {},
                                                     .direction = Direction::Directed,
                                                     .return_edge_refs = false,
                                             },
                                             BfsOptions{
                                                     .max_depth = test_case.max_depth,
                                                     .return_predecessors = true,
                                                     .allow_native_pushdown = false,
                                                     .allow_partial_acceleration = true,
                                             });

            EXPECT_EQ(result.distance.size(), test_case.expected_vertices);
            EXPECT_EQ(result.distance.at(a.global_id), 0U);
            EXPECT_FALSE(result.predecessor.contains(a.global_id));
            if (test_case.expected_vertices >= 3) {
                EXPECT_EQ(result.distance.at(b.global_id), 1U);
                EXPECT_EQ(result.distance.at(c.global_id), 1U);
                EXPECT_EQ(result.predecessor.at(b.global_id), a.global_id);
                EXPECT_EQ(result.predecessor.at(c.global_id), a.global_id);
            }
            EXPECT_EQ(result.distance.contains(d.global_id), test_case.expects_d);
            if (test_case.expects_d) {
                EXPECT_EQ(result.distance.at(d.global_id), 2U);
                ASSERT_TRUE(result.predecessor.contains(d.global_id));
                EXPECT_TRUE(result.predecessor.at(d.global_id) == b.global_id ||
                            result.predecessor.at(d.global_id) == c.global_id);
            }
        }

        INSTANTIATE_TEST_SUITE_P(DepthLimits, BfsExecutorDepthTest,
                                 testing::Values(BfsDepthCase{0U, 1, false}, BfsDepthCase{1U, 3, false},
                                                 BfsDepthCase{2U, 4, true}, BfsDepthCase{std::nullopt, 4, true}));

        TEST_F(BfsExecutorDepthTest, StreamingEmitsEachNonEmptyLayerInOrder) {
            BfsExecutor executor(catalog);
            std::vector<std::uint32_t> depths;
            std::vector<std::size_t> layer_sizes;

            const auto result = executor.run_streaming(a,
                                                       ExpandSpec{
                                                               .allowed_edge_types = {},
                                                               .allowed_target_node_types = {},
                                                               .direction = Direction::Directed,
                                                               .return_edge_refs = false,
                                                       },
                                                       BfsOptions{
                                                               .max_depth = 2,
                                                               .return_predecessors = true,
                                                               .allow_native_pushdown = false,
                                                               .allow_partial_acceleration = true,
                                                       },
                                                       [&](const BfsLayerEvent &layer) {
                                                           depths.push_back(layer.depth);
                                                           layer_sizes.push_back(layer.vertices.size());
                                                       });

            EXPECT_EQ(depths, (std::vector<std::uint32_t>{0, 1, 2}));
            EXPECT_EQ(layer_sizes, (std::vector<std::size_t>{1, 2, 1}));
            EXPECT_EQ(result.distance.size(), 4U);
        }

        TEST_F(BfsExecutorDepthTest, CanOmitPredecessorMap) {
            BfsExecutor executor(catalog);

            const auto result = executor.run(a, ExpandSpec{},
                                             BfsOptions{
                                                     .max_depth = 2,
                                                     .return_predecessors = false,
                                                     .allow_native_pushdown = false,
                                                     .allow_partial_acceleration = true,
                                             });

            EXPECT_TRUE(result.predecessor.empty());
            EXPECT_EQ(result.distance.size(), 4U);
            EXPECT_EQ(result.distance.at(d.global_id), 2U);
        }

        TEST_F(BfsExecutorDepthTest, RejectsEmptyStreamingCallback) {
            BfsExecutor executor(catalog);

            EXPECT_THROW((void) executor.run_streaming(a, ExpandSpec{}, BfsOptions{}, {}), std::invalid_argument);
        }

        TEST(BfsExecutorTest, DirectionOptimizedSwitchesFromSparsePushToBitmapPull) {
            constexpr BackendId pull_backend = MMAP;
            auto registry = std::make_shared<BackendRegistry>();
            const test::TestGraphStore::Graph graph{
                    {a.global_id, {b, c, d}}, {b.global_id, {a, e}},    {c.global_id, {a, e}},
                    {d.global_id, {a, e}},    {e.global_id, {b, c, d}},
            };
            auto owner_store =
                    std::make_shared<test::TestGraphStore>(test::descriptor(backend_id, 10, BackendKind::Neo4j), graph);
            auto pull_store = std::make_shared<test::TestGraphStore>(
                    test::descriptor(pull_backend, 1, BackendKind::MemoryMapped), graph);
            pull_store->enable_pull(node_type);
            registry->register_store(owner_store);
            registry->register_store(pull_store);
            PlacementCatalog catalog(registry);
            catalog.set_id_routing_backends({backend_id});
            catalog.set_cache_residency(std::make_shared<const std::vector<CacheResidencyRecord>>(
                    std::initializer_list<CacheResidencyRecord>{
                            {a.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                            {b.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                            {c.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                            {d.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                            {e.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                    }));
            BfsExecutor executor(catalog);

            BfsOptions options{
                    .max_depth = 2,
                    .return_predecessors = true,
                    .allow_native_pushdown = false,
                    .allow_partial_acceleration = true,
            };
            options.execution_policy = BfsExecutionPolicy::DirectionOptimized;
            options.pull_enter_frontier_percent = 50;
            options.pull_exit_frontier_percent = 10;
            options.bitmap_frontier_min_size = 2;
            options.bitmap_frontier_density_percent = 50;

            const auto result = executor.run(a, ExpandSpec{.direction = Direction::Both}, options);

            EXPECT_EQ(result.distance.size(), 5U);
            EXPECT_EQ(result.distance.at(a.global_id), 0U);
            EXPECT_EQ(result.distance.at(b.global_id), 1U);
            EXPECT_EQ(result.distance.at(c.global_id), 1U);
            EXPECT_EQ(result.distance.at(d.global_id), 1U);
            EXPECT_EQ(result.distance.at(e.global_id), 2U);
            EXPECT_TRUE(result.predecessor.contains(e.global_id));
            EXPECT_EQ(pull_store->neighbor_call_count(), 1U);
            EXPECT_EQ(pull_store->pull_call_count(), 1U);
            EXPECT_EQ(owner_store->neighbor_call_count(), 0U);
        }

        TEST(BfsExecutorTest, ClassicPolicyNeverUsesPullExpansion) {
            constexpr BackendId pull_backend = MMAP;
            auto registry = std::make_shared<BackendRegistry>();
            const test::TestGraphStore::Graph graph{
                    {a.global_id, {b, c}},
                    {b.global_id, {a, d}},
                    {c.global_id, {a, d}},
                    {d.global_id, {b, c}},
            };
            auto owner_store =
                    std::make_shared<test::TestGraphStore>(test::descriptor(backend_id, 10, BackendKind::Neo4j), graph);
            auto pull_store = std::make_shared<test::TestGraphStore>(
                    test::descriptor(pull_backend, 1, BackendKind::MemoryMapped), graph);
            pull_store->enable_pull(node_type);
            registry->register_store(owner_store);
            registry->register_store(pull_store);
            PlacementCatalog catalog(registry);
            catalog.set_id_routing_backends({backend_id});
            catalog.set_cache_residency(std::make_shared<const std::vector<CacheResidencyRecord>>(
                    std::initializer_list<CacheResidencyRecord>{
                            {a.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                            {b.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                            {c.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                            {d.global_id, insertion::BackendEncodedId::MMAP_LOCATION},
                    }));
            BfsExecutor executor(catalog);

            BfsOptions options{
                    .max_depth = 2,
                    .return_predecessors = true,
                    .allow_native_pushdown = false,
                    .allow_partial_acceleration = true,
            };
            options.execution_policy = BfsExecutionPolicy::Classic;

            const auto result = executor.run(a, ExpandSpec{.direction = Direction::Both}, options);

            EXPECT_EQ(result.distance.size(), 4U);
            EXPECT_EQ(result.distance.at(d.global_id), 2U);
            EXPECT_EQ(pull_store->pull_call_count(), 0U);
            EXPECT_EQ(pull_store->neighbor_call_count(), 2U);
            EXPECT_EQ(owner_store->neighbor_call_count(), 0U);
        }

        TEST(BfsExecutorTest, UsesNativePushdownWhenAllowed) {
            constexpr BackendId native_backend = backend_id;
            auto registry = std::make_shared<BackendRegistry>();
            auto store =
                    std::make_shared<test::TestGraphStore>(test::descriptor(native_backend, 1, BackendKind::Neo4j));
            BfsResult native_result;
            native_result.distance.emplace(a.global_id, 0);
            native_result.distance.emplace(d.global_id, 1);
            store->enable_native_bfs(native_result);
            registry->register_store(store);
            PlacementCatalog catalog(registry);
            catalog.set_id_routing_backends({backend_id});
            BfsExecutor executor(catalog);

            const auto result = executor.run(a, ExpandSpec{},
                                             BfsOptions{
                                                     .max_depth = std::nullopt,
                                                     .return_predecessors = true,
                                                     .allow_native_pushdown = true,
                                                     .allow_partial_acceleration = true,
                                             });

            EXPECT_EQ(result.distance, native_result.distance);
            EXPECT_EQ(store->native_bfs_call_count(), 1U);
            EXPECT_EQ(store->neighbor_call_count(), 0U);
        }

    } // namespace
} // namespace polygraph
