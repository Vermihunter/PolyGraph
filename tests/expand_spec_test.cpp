#include "algorithms/ExpandSpec.hpp"

#include <gtest/gtest.h>
#include <tuple>
#include <vector>

namespace polygraph {
    namespace {

        struct TypeFilterCase {
            std::vector<TypeId> allowed;
            TypeId candidate;
            bool expected;
        };

        class EdgeTypeFilterTest : public testing::TestWithParam<TypeFilterCase> {};

        TEST_P(EdgeTypeFilterTest, AppliesAllowListSemantics) {
            const auto &test_case = GetParam();
            const ExpandSpec spec{
                    .allowed_edge_types = test_case.allowed,
                    .allowed_target_node_types = {},
                    .direction = Direction::Both,
                    .return_edge_refs = false,
            };

            EXPECT_EQ(spec.allows_edge_type(test_case.candidate), test_case.expected);
        }

        INSTANTIATE_TEST_SUITE_P(EdgeTypeCases, EdgeTypeFilterTest,
                                 testing::Values(TypeFilterCase{{}, 7, true}, TypeFilterCase{{7}, 7, true},
                                                 TypeFilterCase{{1, 7, 9}, 7, true}, TypeFilterCase{{1, 9}, 7, false}));

        class TargetTypeFilterTest : public testing::TestWithParam<TypeFilterCase> {};

        TEST_P(TargetTypeFilterTest, AppliesAllowListSemantics) {
            const auto &test_case = GetParam();
            const ExpandSpec spec{
                    .allowed_edge_types = {},
                    .allowed_target_node_types = test_case.allowed,
                    .direction = Direction::Both,
                    .return_edge_refs = false,
            };

            EXPECT_EQ(spec.allows_target_type(test_case.candidate), test_case.expected);
        }

        INSTANTIATE_TEST_SUITE_P(TargetTypeCases, TargetTypeFilterTest,
                                 testing::Values(TypeFilterCase{{}, 4, true}, TypeFilterCase{{4}, 4, true},
                                                 TypeFilterCase{{2, 4, 8}, 4, true}, TypeFilterCase{{2, 8}, 4, false}));

    } // namespace
} // namespace polygraph
