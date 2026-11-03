#include "infrastructure/BackendRegistry.hpp"
#include "support/TestGraphStore.hpp"
#include "utilities/BackendIds.hpp"

#include <gtest/gtest.h>
#include <memory>

namespace polygraph {
    namespace {

        TEST(BackendRegistryTest, RejectsNullStores) {
            BackendRegistry registry;

            EXPECT_THROW(registry.register_store(nullptr), std::invalid_argument);
        }

        TEST(BackendRegistryTest, RegistersAndRetrievesStoreAndDescriptor) {
            constexpr BackendId backend_id = NEO4J;
            auto store = std::make_shared<test::TestGraphStore>(test::descriptor(backend_id, 25, BackendKind::Neo4j));
            BackendRegistry registry;

            registry.register_store(store);

            EXPECT_TRUE(registry.contains(backend_id));
            EXPECT_EQ(registry.get_store(backend_id), store);
            EXPECT_EQ(registry.descriptor(backend_id).priority, 25);
            EXPECT_EQ(registry.stores().size(), 1U);
            EXPECT_EQ(registry.descriptors().size(), 1U);
        }

        TEST(BackendRegistryTest, ReplacesStoreWithSameBackendId) {
            constexpr BackendId backend_id = REDIS;
            auto first =
                    std::make_shared<test::TestGraphStore>(test::descriptor(backend_id, 50, BackendKind::RamCache));
            auto replacement =
                    std::make_shared<test::TestGraphStore>(test::descriptor(backend_id, 5, BackendKind::RedisCache));
            BackendRegistry registry;

            registry.register_store(first);
            registry.register_store(replacement);

            EXPECT_EQ(registry.get_store(backend_id), replacement);
            EXPECT_EQ(registry.descriptor(backend_id).priority, 5);
            EXPECT_EQ(registry.descriptor(backend_id).kind, BackendKind::RedisCache);
            EXPECT_EQ(registry.stores().size(), 1U);
            EXPECT_EQ(registry.descriptors().size(), 1U);
        }

        TEST(BackendRegistryTest, ReportsUnknownBackend) {
            BackendRegistry registry;

            EXPECT_FALSE(registry.contains(404));
            EXPECT_THROW((void) registry.get_store(404), std::runtime_error);
            EXPECT_THROW((void) registry.descriptor(404), std::out_of_range);
        }

    } // namespace
} // namespace polygraph
