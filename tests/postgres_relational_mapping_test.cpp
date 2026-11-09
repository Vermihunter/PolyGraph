#include <gtest/gtest.h>
#include "data_access/adapters/PostgreSQL/RelationalMapping.hpp"

using namespace polygraph;
using namespace polygraph::postgres;

namespace {
    domain::LogicalGraphSchema schema() {
        domain::LogicalGraphSchema s;
        const auto person = s.add_node_type("Person", {{"name", domain::PropertyType::string}});
        const auto post = s.add_node_type("Post");
        s.add_edge_type("Likes", person, post, true, {{"weight", domain::PropertyType::integer}});
        s.add_edge_type("Author", post, person);
        return s;
    }
    json mapping() {
        return {{"format", "polygraph.postgresql-relational"},
                {"version", 1},
                {"nodes", json::array({{{"type", "Person"},
                                        {"schema", "app"},
                                        {"table", "person"},
                                        {"primaryKey", "id"},
                                        {"namespace", "application"},
                                        {"keyCodec", "int64"},
                                        {"properties", {{"name", "display_name"}}}},
                                       {{"type", "Post"},
                                        {"schema", "app"},
                                        {"table", "post"},
                                        {"primaryKey", "id"},
                                        {"namespace", "application"},
                                        {"keyCodec", "uuid"}}})},
                {"relationships",
                 json::array(
                         {{{"type", "Likes"},
                           {"kind", "table"},
                           {"schema", "app"},
                           {"table", "likes"},
                           {"fromColumn", "person_id"},
                           {"toColumn", "post_id"},
                           {"properties", {{"weight", "weight"}}}},
                          {{"type", "Author"}, {"kind", "foreignKey"}, {"owner", "from"}, {"column", "author_id"}}})}};
    }
    void rejects(json j, std::string_view fragment) {
        try {
            (void) parse_relational_mapping(j, schema());
            FAIL() << "accepted invalid mapping";
        } catch (const std::invalid_argument &e) {
            EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos) << e.what();
        }
    }
} // namespace

TEST(RelationalMapping, ParsesBothRelationshipFormsAndTypedProperties) {
    const auto m = parse_relational_mapping(mapping(), schema());
    ASSERT_EQ(m.nodes().size(), 2);
    EXPECT_EQ(m.nodes()[0].table.schema, "app");
    EXPECT_EQ(m.nodes()[1].key_codec, KeyCodec::uuid);
    ASSERT_EQ(m.relationships().size(), 2);
    EXPECT_EQ(m.relationships()[0].from_type, m.nodes()[0].type);
    EXPECT_EQ(m.relationships()[1].kind, RelationshipKind::foreign_key);
    EXPECT_EQ(m.relationships()[1].owner, ForeignKeyOwner::from);
}
TEST(RelationalMapping, RejectsDuplicateAndUnknownLogicalMappings) {
    auto j = mapping();
    j["nodes"].push_back(j["nodes"][0]);
    rejects(j, "duplicate");
    j = mapping();
    j["nodes"][0]["type"] = "Missing";
    rejects(j, "Missing");
    j = mapping();
    j["relationships"].push_back(j["relationships"][0]);
    rejects(j, "duplicate");
    j = mapping();
    j["nodes"].erase(0);
    rejects(j, "endpoint");
}
TEST(RelationalMapping, RejectsUnknownFieldsVersionsAndUnsupportedKeys) {
    auto j = mapping();
    j["version"] = 2;
    rejects(j, "version");
    j = mapping();
    j["version"] = 1.0;
    rejects(j, "version");
    j = mapping();
    j["nodes"][0]["primaryKey"] = json::array({"a", "b"});
    rejects(j, "primaryKey");
    j = mapping();
    j["nodes"][0]["keyCodec"] = "float";
    rejects(j, "keyCodec");
    j = mapping();
    j["nodes"][0]["sql"] = "SELECT 1";
    rejects(j, "sql");
    j = mapping();
    j["relationships"][1]["table"] = "post";
    rejects(j, "table");
}
TEST(RelationalMapping, RejectsInvalidPropertiesAndAmbiguousTables) {
    auto j = mapping();
    j["nodes"][0]["properties"] = {{"missing", "x"}};
    rejects(j, "property");
    j = mapping();
    j["nodes"][1]["table"] = "person";
    rejects(j, "table");
    j = mapping();
    j["relationships"][1]["properties"] = {{"weight", "x"}};
    rejects(j, "properties");
}
TEST(RelationalMapping, PreservesIdentifiersWithoutSqlInterpretation) {
    auto j = mapping();
    j["nodes"][0]["table"] = "Person.µ\"; DROP TABLE sentinel;--";
    const auto m = parse_relational_mapping(j, schema());
    EXPECT_EQ(m.nodes()[0].table.name, j["nodes"][0]["table"]);
    EXPECT_EQ(quote_identifier("a\"b.c"), "\"a\"\"b.c\"");
    j["nodes"][0]["schema"] = std::string("a\0b", 3);
    rejects(j, "schema");
    j = mapping();
    j["nodes"][0]["table"] = std::string(64, 'x');
    rejects(j, "table");
}
TEST(RelationalMapping, ResolvesReferenceRelativeToContainingFile) {
    json config{{"relationalMapping", "mappings/app.json"}};
    EXPECT_EQ(relational_mapping_path(config, "/tmp/config/backends.json"),
              std::filesystem::path("/tmp/config/mappings/app.json"));
    EXPECT_FALSE(relational_mapping_path(json::object(), "/tmp/a.json"));
    EXPECT_THROW(relational_mapping_path({{"relationalMapping", ""}}, "/tmp/a.json"), std::invalid_argument);
}

TEST(RelationalMapping, LoadsFilesWithActionablePathErrors) {
    EXPECT_THROW((void) load_relational_mapping("/missing/polygraph-mapping.json", schema()), std::invalid_argument);
}
