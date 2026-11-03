#include "serialization/RecordJsonSerializer.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace polygraph {
    namespace {

        TEST(RecordJsonSerializerTest, RoundTripsNodeProperties) {
            Node node{
                    .global_id = 0x8000000000000101ULL,
                    .type = 4,
                    .labels = {"Person", "Student"},
                    .properties = {},
            };
            node.properties.emplace("name", std::make_unique<Value>(std::string{"Akos"}));
            node.properties.emplace("age", std::make_unique<Value>(std::int64_t{24}));

            const json serialized = node;
            const Node restored = serialized.get<Node>();

            EXPECT_EQ(restored.global_id, node.global_id);
            EXPECT_EQ(restored.type, node.type);
            EXPECT_EQ(restored.labels, node.labels);
            ASSERT_TRUE(restored.properties.contains("name"));
            ASSERT_TRUE(restored.properties.at("name")->is<std::string>());
            EXPECT_EQ(restored.properties.at("name")->as<std::string>(), "Akos");
            ASSERT_TRUE(restored.properties.at("age")->is<std::int64_t>());
            EXPECT_EQ(restored.properties.at("age")->as<std::int64_t>(), 24);
        }

        TEST(RecordJsonSerializerTest, DetectsStructuredValueKinds) {
            const json node_json{
                    {"id", GlobalId{102}},
                    {"type", 2},
                    {"labels", json::array({"Course"})},
                    {"properties", json::object()},
            };
            const json edge_json{
                    {"global_id", GlobalId{103}},   {"type", 8}, {"src", GlobalId{101}}, {"dst", GlobalId{102}},
                    {"properties", json::object()},
            };
            const json path_json{
                    {"nodes", json::array()},
                    {"edges", json::array()},
            };

            EXPECT_TRUE(node_json.get<Value>().is<Node>());
            EXPECT_TRUE(edge_json.get<Value>().is<Edge>());
            EXPECT_TRUE(path_json.get<Value>().is<Path>());
        }

        TEST(RecordJsonSerializerTest, RoundTripsRecordFields) {
            Record record;
            record.fields.emplace_back(std::int64_t{7});
            record.fields.emplace_back(std::string{"value"});
            record.fields.emplace_back(true);

            const json serialized = record;
            Record restored = serialized.get<Record>();

            ASSERT_EQ(restored.fields.size(), 3U);
            EXPECT_EQ(restored.fields[0].as<std::int64_t>(), 7);
            EXPECT_EQ(restored.fields[1].as<std::string>(), "value");
            EXPECT_TRUE(restored.fields[2].as<bool>());
        }

        class JsonGlobalIdTest : public testing::TestWithParam<json> {};

        TEST_P(JsonGlobalIdTest, RestoresUnsignedIdentityFromSupportedStorageForms) {
            constexpr GlobalId expected = 0x800000000000002AULL;
            const json input{{"id", GetParam()},
                             {"type", 4},
                             {"labels", json::array({"Person"})},
                             {"properties", json::object()}};

            const Node restored = input.get<Node>();
            const json serialized = restored;

            EXPECT_EQ(restored.global_id, expected);
            ASSERT_TRUE(serialized.at("id").is_number_unsigned());
            EXPECT_EQ(serialized.at("id").get<GlobalId>(), expected);
        }

        INSTANTIATE_TEST_SUITE_P(NumericAndDecimalStorage, JsonGlobalIdTest,
                                 testing::Values(json(GlobalId{0x800000000000002AULL}),
                                                 json(std::numeric_limits<std::int64_t>::min() + 42),
                                                 json("9223372036854775850")));

        TEST(RecordJsonSerializerTest, RoundTripsEdgeIdsAboveSignedIntegerRange) {
            const Edge edge{.global_id = 0x8000000000000003ULL,
                            .type = 8,
                            .src = 0x8000000000000001ULL,
                            .dst = 0x0002000000000002ULL,
                            .properties = {}};

            const json serialized = edge;
            const Edge restored = serialized.get<Edge>();

            EXPECT_EQ(restored.global_id, edge.global_id);
            EXPECT_EQ(restored.src, edge.src);
            EXPECT_EQ(restored.dst, edge.dst);
            EXPECT_EQ(restored.type, edge.type);
        }

        TEST(RecordJsonSerializerTest, RejectsNonNumericNodeIds) {
            for (const json &id: {json(nullptr), json(true), json::array(), json::object()}) {
                const json input{{"id", id}, {"labels", json::array()}, {"properties", json::object()}};
                EXPECT_THROW((void) input.get<Node>(), std::invalid_argument);
            }
        }

    } // namespace
} // namespace polygraph
