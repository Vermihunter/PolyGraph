#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "cli/BackendConfigParser.hpp"
#include "infrastructure/ConfiguredGraphStoreFactory.hpp"
#include "utilities/BackendIds.hpp"

using namespace polygraph;

TEST(RelationalConfig, RejectsMappingOnOtherBackendKinds) {
    const json j{{"relationalMapping", "/tmp/mapping.json"}};
    EXPECT_THROW((void) config::parse_connection_config(j, NEO4J, BackendKind::Neo4j), std::invalid_argument);
}
TEST(RelationalConfig, RejectsRelativeReferenceWithoutContainingFile) {
    EXPECT_THROW((void) config::parse_connection_config({{"relationalMapping", "mapping.json"}}, POSTGRESQL,
                                                        BackendKind::PostgreSQL),
                 std::invalid_argument);
}
TEST(RelationalConfig, RejectsIdentityWithoutMappingOrOnOtherKindsOrInsideConnection) {
    EXPECT_THROW((void) config::parse_connection_config({{"relationalIdentity", "/tmp/catalog.json"}}, POSTGRESQL,
                                                        BackendKind::PostgreSQL),
                 std::invalid_argument);
    EXPECT_THROW((void) config::parse_connection_config({{"relationalIdentity", "/tmp/catalog.json"}}, NEO4J,
                                                        BackendKind::Neo4j),
                 std::invalid_argument);
    EXPECT_THROW((void) config::parse_connection_config({{"connection", {{"relationalIdentity", "/tmp/catalog.json"}}}},
                                                        POSTGRESQL, BackendKind::PostgreSQL),
                 std::invalid_argument);
}
TEST(RelationalConfig, PreservesGenericConfigurationAndAbsoluteMapping) {
    const auto generic = config::parse_connection_config(json::object(), POSTGRESQL, BackendKind::PostgreSQL);
    EXPECT_FALSE(generic.relational_mapping);
    const auto relational = config::parse_connection_config({{"relationalMapping", "/tmp/mapping.json"}}, POSTGRESQL,
                                                            BackendKind::PostgreSQL);
    ASSERT_TRUE(relational.relational_mapping);
    EXPECT_EQ(*relational.relational_mapping, "/tmp/mapping.json");
}
TEST(RelationalConfig, ResolvesFileReferencesAgainstConfigDirectory) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("polygraph-relational-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove(path); }
    } cleanup{path};
    std::ofstream(path)
            << R"({"backends":[{"id":"PostgreSQL","relationalMapping":"maps/application.json","relationalIdentity":"catalog/manifest.json"}]})";
    const auto root = config::load_backend_config_file_or_default(path.string());
    EXPECT_EQ(root["backends"][0]["relationalMapping"], (path.parent_path() / "maps/application.json").string());
    EXPECT_EQ(root["backends"][0]["relationalIdentity"], (path.parent_path() / "catalog/manifest.json").string());
}
TEST(RelationalConfig, DoesNotSilentlySelectGenericTraversal) {
    auto context = DatabaseContext::create_empty();
    domain::LogicalGraphSchema schema;
    const std::unordered_map<BackendId, std::unordered_set<TypeId>> types;
    ConfiguredGraphStoreFactory::Inputs inputs{
            .data_path = "", .schema = schema, .nodes_by_backend = types, .edges_by_backend = types};
    const json backend{{"id", "PostgreSQL"},
                       {"kind", "PostgreSQL"},
                       {"relationalMapping", "/missing/mapping.json"},
                       {"connection", {{"endpoint", "localhost:5432"}}}};
    try {
        (void) ConfiguredGraphStoreFactory::create_from_json(*context, backend, inputs);
        FAIL() << "silently selected generic traversal";
    } catch (const std::exception &e) {
        EXPECT_NE(std::string(e.what()).find("mapping"), std::string::npos);
    }
}

TEST(RelationalConfig, ValidMappingRequiresExplicitIdentityCatalog) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("polygraph-valid-relational-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove(path); }
    } cleanup{path};
    std::ofstream(path)
            << R"({"format":"polygraph.postgresql-relational","version":1,"nodes":[{"type":"Person","schema":"app","table":"person","primaryKey":"id","namespace":"app","keyCodec":"int64"}],"relationships":[]})";
    auto context = DatabaseContext::create_empty();
    domain::LogicalGraphSchema schema;
    schema.add_node_type("Person");
    const std::unordered_map<BackendId, std::unordered_set<TypeId>> types;
    ConfiguredGraphStoreFactory::Inputs inputs{
            .data_path = "", .schema = schema, .nodes_by_backend = types, .edges_by_backend = types};
    const json backend{{"id", "PostgreSQL"},
                       {"kind", "PostgreSQL"},
                       {"relationalMapping", path.string()},
                       {"connection", {{"endpoint", "localhost:5432"}}}};
    try {
        (void) ConfiguredGraphStoreFactory::create_from_json(*context, backend, inputs);
        FAIL() << "selected generic traversal";
    } catch (const std::exception &e) {
        EXPECT_NE(std::string(e.what()).find("relationalIdentity"), std::string::npos) << e.what();
    }
}
