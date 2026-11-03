#include "serialization/DataModelJsonSerializer.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace polygraph {
    namespace {

        using Primitive = std::variant<bool, std::int64_t, double, std::string>;

        class JsonPrimitiveTest : public testing::TestWithParam<Primitive> {};

        TEST_P(JsonPrimitiveTest, PreservesSupportedPrimitiveValues) {
            const Primitive &expected = GetParam();
            const json input = std::visit([](const auto &value) { return json(value); }, expected);

            const auto result = DataModelJsonSerializer::json_primitive_to_value(input);

            ASSERT_NE(result, nullptr);
            std::visit(
                    [&result](const auto &value) {
                        using ValueType = std::decay_t<decltype(value)>;
                        ASSERT_TRUE(result->is<ValueType>());
                        if constexpr (std::is_same_v<ValueType, double>) {
                            EXPECT_DOUBLE_EQ(result->as<ValueType>(), value);
                        } else {
                            EXPECT_EQ(result->as<ValueType>(), value);
                        }
                    },
                    expected);
        }

        INSTANTIATE_TEST_SUITE_P(SupportedValues, JsonPrimitiveTest,
                                 testing::Values(Primitive{false}, Primitive{true}, Primitive{std::int64_t{-42}},
                                                 Primitive{std::int64_t{0}}, Primitive{std::int64_t{42}},
                                                 Primitive{std::numeric_limits<std::int64_t>::min()},
                                                 Primitive{std::numeric_limits<std::int64_t>::max()}, Primitive{3.25},
                                                 Primitive{std::string{"graph"}}, Primitive{std::string{}}));

        TEST(DataModelJsonSerializerTest, AcceptsLargestUnsignedValueThatFitsInt64) {
            const auto input = json(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));

            const auto result = DataModelJsonSerializer::json_primitive_to_value(input);

            ASSERT_NE(result, nullptr);
            ASSERT_TRUE(result->is<std::int64_t>());
            EXPECT_EQ(result->as<std::int64_t>(), std::numeric_limits<std::int64_t>::max());
        }

        TEST(DataModelJsonSerializerTest, RejectsUnsignedIntegerOverflow) {
            const auto input = json(std::numeric_limits<std::uint64_t>::max());

            EXPECT_THROW((void) DataModelJsonSerializer::json_primitive_to_value(input), std::out_of_range);
        }

        TEST(DataModelJsonSerializerTest, ReturnsNullForNonPrimitiveJson) {
            EXPECT_EQ(DataModelJsonSerializer::json_primitive_to_value(json::array()), nullptr);
            EXPECT_EQ(DataModelJsonSerializer::json_primitive_to_value(json::object()), nullptr);
            EXPECT_EQ(DataModelJsonSerializer::json_primitive_to_value(nullptr), nullptr);
        }

    } // namespace
} // namespace polygraph
