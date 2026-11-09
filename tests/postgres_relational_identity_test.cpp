#include <filesystem>
#include <gtest/gtest.h>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"

using namespace polygraph;
using namespace polygraph::postgres;

TEST(RelationalIdentity, PreservesExactScalarKeysAndTypedSourceEncoding) {
    NodeMapping node{.type = 1,
                     .logical_type = "Person",
                     .table = {"app", "person"},
                     .primary_key = "id",
                     .name_space = "app",
                     .key_codec = KeyCodec::int64,
                     .properties = {}};
    const auto key = relational_source_identity(node, json(std::int64_t{-9223372036854775807LL}));
    EXPECT_TRUE(key.value.is_number_integer());
    EXPECT_FALSE(key.value.is_number_unsigned());
    EXPECT_EQ(key.value, -9223372036854775807LL);
    EXPECT_THROW((void) relational_source_identity(node, "7"), std::invalid_argument);
    EXPECT_THROW((void) relational_source_identity(node, json(std::uint64_t{9223372036854775808ULL})),
                 std::invalid_argument);
    node.key_codec = KeyCodec::string;
    EXPECT_EQ(relational_source_identity(node, "007").value, "007");
    EXPECT_THROW((void) relational_source_identity(node, 7), std::invalid_argument);
}
TEST(RelationalIdentity, CanonicalizesUuidStringsAndRejectsInvalidKeys) {
    NodeMapping node{.type = 1,
                     .logical_type = "Post",
                     .table = {"app", "post"},
                     .primary_key = "id",
                     .name_space = "app",
                     .key_codec = KeyCodec::uuid,
                     .properties = {}};
    EXPECT_EQ(relational_source_identity(node, "A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11").value,
              "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
    EXPECT_THROW((void) relational_source_identity(node, "not-a-uuid"), std::invalid_argument);
    EXPECT_THROW((void) relational_source_identity(node, nullptr), std::invalid_argument);
    EXPECT_THROW((void) relational_source_identity(node, "a0eebc999c0b4ef8bb6d6bb9bd380a11"), std::invalid_argument);
}
TEST(RelationalIdentity, FingerprintIgnoresMappingOrderButBindsPhysicalAndIdentityContract) {
    domain::LogicalGraphSchema schema;
    schema.add_node_type("Person");
    schema.add_node_type("Post");
    json j{{"format", "polygraph.postgresql-relational"},
           {"version", 1},
           {"relationships", json::array()},
           {"nodes", json::array({{{"type", "Person"},
                                   {"schema", "app"},
                                   {"table", "person"},
                                   {"primaryKey", "id"},
                                   {"namespace", "app"},
                                   {"keyCodec", "int64"}},
                                  {{"type", "Post"},
                                   {"schema", "app"},
                                   {"table", "post"},
                                   {"primaryKey", "id"},
                                   {"namespace", "app"},
                                   {"keyCodec", "int64"}}})}};
    const auto expected = relational_mapping_fingerprint(parse_relational_mapping(j, schema));
    std::swap(j["nodes"][0], j["nodes"][1]);
    EXPECT_EQ(relational_mapping_fingerprint(parse_relational_mapping(j, schema)), expected);
    j["nodes"][0]["namespace"] = "different";
    EXPECT_NE(relational_mapping_fingerprint(parse_relational_mapping(j, schema)), expected);
}
