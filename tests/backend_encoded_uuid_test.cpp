#include "insertion/BackendEncodedUuid.hpp"
#include "utilities/BackendIds.hpp"

#include <array>
#include <gtest/gtest.h>
#include <stdexcept>

namespace polygraph::insertion {
    namespace {

        constexpr std::array routing{NEO4J, ARANGO, MEMGRAPH, POSTGRESQL, GPU};

        class BackendEncodedIdTest : public testing::TestWithParam<BackendId> {};

        TEST_P(BackendEncodedIdTest, GeneratedIdsRoundTripTheirOwner) {
            const BackendId owner = GetParam();
            const GlobalId id = BackendEncodedId::generate(owner, routing);

            EXPECT_NE(id, invalid_global_id);
            EXPECT_EQ(BackendEncodedId::decode(id, routing), owner);
            EXPECT_LE(BackendEncodedId::local_id(id, routing), BackendEncodedId::payload_mask());
            EXPECT_EQ(BackendEncodedId::canonical_id(id), id);
        }

        INSTANTIATE_TEST_SUITE_P(PersistentAndGpuOwners, BackendEncodedIdTest,
                                 testing::Values(NEO4J, ARANGO, MEMGRAPH, POSTGRESQL, GPU, gpu_backend_id(9)));

        TEST(BackendEncodedIdLayoutTest, HasFixedSixteenBitLocationsAndFortyEightBitPayload) {
            EXPECT_EQ(BackendEncodedId::bit_count(routing.size()), 16U);
            EXPECT_EQ(BackendEncodedId::payload_bit_count(routing.size()), 48U);
            EXPECT_EQ(BackendEncodedId::payload_mask(routing.size()), 0x0000FFFFFFFFFFFFULL);
            EXPECT_THROW((void) BackendEncodedId::bit_count(0), std::invalid_argument);
            EXPECT_THROW((void) BackendEncodedId::payload_bit_count(0), std::invalid_argument);
            EXPECT_THROW((void) BackendEncodedId::payload_mask(0), std::invalid_argument);
        }

        TEST(BackendEncodedIdLayoutTest, EncodesKnownOwnerBitPatternsAtPayloadBoundaries) {
            EXPECT_EQ(BackendEncodedId::from_local_id(0, NEO4J, routing), 0x0001000000000000ULL);
            EXPECT_EQ(BackendEncodedId::from_local_id(42, ARANGO, routing), 0x000200000000002AULL);
            EXPECT_EQ(BackendEncodedId::from_local_id(42, POSTGRESQL, routing), 0x800000000000002AULL);
            EXPECT_EQ(BackendEncodedId::from_local_id(BackendEncodedId::payload_mask(), POSTGRESQL, routing),
                      0x8000FFFFFFFFFFFFULL);
            EXPECT_THROW((void) BackendEncodedId::from_local_id(GlobalId{1} << 48U, NEO4J, routing),
                         std::overflow_error);
        }

        TEST(BackendEncodedIdLayoutTest, CacheResidencyDoesNotChangeCanonicalIdentityOrOwner) {
            const GlobalId canonical = BackendEncodedId::from_local_id(73, POSTGRESQL, routing);
            constexpr auto residency = BackendEncodedId::REDIS_LOCATION | BackendEncodedId::MMAP_LOCATION;
            const GlobalId decorated = BackendEncodedId::with_cache_residency(canonical, residency);

            EXPECT_NE(decorated, canonical);
            EXPECT_EQ(BackendEncodedId::canonical_id(decorated), canonical);
            EXPECT_EQ(BackendEncodedId::decode(decorated, routing), POSTGRESQL);
            EXPECT_EQ(BackendEncodedId::cache_residency(decorated), residency);
            EXPECT_TRUE(BackendEncodedId::has_location(decorated, BackendEncodedId::REDIS_LOCATION));
            EXPECT_EQ(BackendEncodedId::with_cache_residency(decorated, 0), canonical);
        }

        TEST(BackendEncodedIdLayoutTest, ReencodingPreservesPayloadAndCacheLocations) {
            constexpr auto residency = BackendEncodedId::MMAP_LOCATION | (BackendEncodedId::LocationMask{1} << 7U);
            const GlobalId original = BackendEncodedId::from_local_id(0x123456789ABCULL, ARANGO, routing, residency);
            const GlobalId encoded = BackendEncodedId::encode(original, NEO4J, routing);

            EXPECT_EQ(BackendEncodedId::decode(encoded, routing), NEO4J);
            EXPECT_EQ(BackendEncodedId::local_id(encoded, routing), 0x123456789ABCULL);
            EXPECT_EQ(BackendEncodedId::cache_residency(encoded), residency);
            EXPECT_EQ(BackendEncodedId::location_mask(encoded), BackendEncodedId::NEO4J_LOCATION | residency);
        }

        TEST(BackendEncodedIdLayoutTest, GpuOwnerAndAdditionalGpuResidencyRemainDistinct) {
            const GlobalId id = BackendEncodedId::from_local_id(5, gpu_backend_id(2), routing,
                                                                BackendEncodedId::gpu_location_bit(5));

            EXPECT_EQ(BackendEncodedId::decode(id, routing), gpu_backend_id(2));
            EXPECT_EQ(BackendEncodedId::owner_location(id), BackendEncodedId::gpu_location_bit(2));
            EXPECT_EQ(BackendEncodedId::cache_residency(id), BackendEncodedId::gpu_location_bit(5));
            EXPECT_THROW((void) BackendEncodedId::gpu_location_bit(BackendEncodedId::MAX_GPU_DEVICES),
                         std::out_of_range);
        }

        TEST(BackendEncodedIdLayoutTest, RejectsMissingOrAmbiguousAuthoritativeLocations) {
            constexpr GlobalId cache_only = GlobalId{BackendEncodedId::REDIS_LOCATION} << 48U;
            constexpr GlobalId two_owners =
                    GlobalId{BackendEncodedId::NEO4J_LOCATION | BackendEncodedId::ARANGO_LOCATION} << 48U;
            EXPECT_THROW((void) BackendEncodedId::decode(0, routing), std::invalid_argument);
            EXPECT_THROW((void) BackendEncodedId::decode(cache_only, routing), std::invalid_argument);
            EXPECT_THROW((void) BackendEncodedId::decode(two_owners, routing), std::invalid_argument);
            EXPECT_FALSE(BackendEncodedId::try_decode(two_owners, routing).has_value());
        }

        TEST(BackendEncodedIdLayoutTest, RejectsInvalidLayoutsAndUnconfiguredOwners) {
            constexpr std::array<BackendId, 0> empty{};
            constexpr std::array duplicate{NEO4J, NEO4J};
            constexpr std::array neo4j_only{NEO4J};
            const GlobalId arango = BackendEncodedId::from_local_id(2, ARANGO, routing);

            EXPECT_THROW((void) BackendEncodedId::from_local_id(1, NEO4J, empty), std::invalid_argument);
            EXPECT_THROW((void) BackendEncodedId::from_local_id(1, NEO4J, duplicate), std::invalid_argument);
            EXPECT_THROW((void) BackendEncodedId::from_local_id(1, ARANGO, neo4j_only), std::invalid_argument);
            EXPECT_THROW((void) BackendEncodedId::decode(arango, neo4j_only), std::invalid_argument);
            EXPECT_THROW((void) BackendEncodedId::from_local_id(1, REDIS, routing), std::invalid_argument);
            EXPECT_FALSE(BackendEncodedId::try_decode(arango, empty).has_value());
        }

    } // namespace
} // namespace polygraph::insertion
