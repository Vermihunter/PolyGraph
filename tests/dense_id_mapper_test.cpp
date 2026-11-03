#include "gpu/data_model/DenseIdMapper.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <vector>

namespace polygraph {
    namespace {

        TEST(DenseIdMapperTest, SortsAndDeduplicatesIdsBeforeAssigningDenseIndices) {
            const DenseIdMapper<std::int32_t> mapper({91, 7, 91, 42, 7});

            EXPECT_EQ(mapper.dense_to_global(), (std::vector<GlobalId>{7, 42, 91}));
            EXPECT_EQ(mapper.size(), 3U);
            EXPECT_EQ(mapper.at(7), 0);
            EXPECT_EQ(mapper.at(42), 1);
            EXPECT_EQ(mapper.at(91), 2);
            EXPECT_EQ(mapper.global_id(1), 42U);
            EXPECT_EQ(mapper.required_bytes(), 3U * sizeof(GlobalId));
            EXPECT_GE(mapper.allocated_bytes(), mapper.required_bytes());
        }

        TEST(DenseIdMapperTest, PreservesFullUnsignedIdsIncludingPostgresqlOwnerBit) {
            constexpr GlobalId postgres_id = 0x800000000000002AULL;
            constexpr GlobalId largest_id = std::numeric_limits<GlobalId>::max();
            const DenseIdMapper<std::int32_t> mapper({largest_id, postgres_id, 0});

            EXPECT_EQ(mapper.global_id(0), 0U);
            EXPECT_EQ(mapper.global_id(1), postgres_id);
            EXPECT_EQ(mapper.global_id(2), largest_id);
            EXPECT_EQ(mapper.at(postgres_id), 1);
            EXPECT_EQ(mapper.at(largest_id), 2);
        }

        TEST(DenseIdMapperTest, RejectsMissingGlobalAndOutOfRangeDenseIds) {
            const DenseIdMapper<std::int32_t> mapper({7, 42});

            EXPECT_TRUE(mapper.contains(7));
            EXPECT_FALSE(mapper.contains(8));
            EXPECT_THROW((void) mapper.at(8), std::runtime_error);
            EXPECT_THROW((void) mapper.global_id(-1), std::out_of_range);
            EXPECT_THROW((void) mapper.global_id(2), std::out_of_range);
        }

        TEST(DenseIdMapperTest, HandlesAnEmptyMapping) {
            const DenseIdMapper<std::int32_t> mapper({});

            EXPECT_EQ(mapper.size(), 0U);
            EXPECT_EQ(mapper.required_bytes(), 0U);
            EXPECT_FALSE(mapper.contains(0));
            EXPECT_THROW((void) mapper.at(0), std::runtime_error);
            EXPECT_THROW((void) mapper.global_id(0), std::out_of_range);
        }

        TEST(DenseIdMapperTest, AcceptsValidatedPrebuiltMappingAndRejectsMalformedArtifacts) {
            const auto mapper = DenseIdMapper<std::int32_t>::from_sorted_unique({4, 17, 99});

            EXPECT_EQ(mapper.at(17), 1);
            EXPECT_EQ(mapper.global_id(2), 99U);
            EXPECT_THROW((void) DenseIdMapper<std::int32_t>::from_sorted_unique({17, 4}), std::invalid_argument);
            EXPECT_THROW((void) DenseIdMapper<std::int32_t>::from_sorted_unique({4, 4}), std::invalid_argument);
        }

        TEST(DenseIdMapperTest, EnforcesDenseTypeCapacityAfterDeduplication) {
            std::vector<GlobalId> ids(129);
            for (std::size_t index = 0; index < ids.size(); ++index) {
                ids[index] = index;
            }

            EXPECT_THROW((void) DenseIdMapper<std::int8_t>(ids), std::overflow_error);
            EXPECT_THROW((void) DenseIdMapper<std::int8_t>::from_sorted_unique(ids), std::overflow_error);
            ids.pop_back();
            ids.pop_back();
            EXPECT_NO_THROW((void) DenseIdMapper<std::int8_t>(ids));
            EXPECT_NO_THROW((void) DenseIdMapper<std::int8_t>(std::vector<GlobalId>(128, 1)));
        }

    } // namespace
} // namespace polygraph
