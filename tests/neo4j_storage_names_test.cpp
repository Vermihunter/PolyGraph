#include <gtest/gtest.h>
#include "data_access/adapters/Neo4J/response_processors/LocallyDeployedNeo4JResponseProcessor.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "utilities/BackendIds.hpp"

namespace polygraph {
    TEST(Neo4jStorageNamesTest, RestoresLogicalTypesFromStoredLabelsAndRelationships) {
        auto context = DatabaseContext::create_empty();
        context->id_routing_backends = {NEO4J, ARANGO, POSTGRESQL};
        const auto device = context->schema->add_node_type("Device-unit");
        const auto site = context->schema->add_node_type("Site/office");
        const auto edge = context->schema->add_edge_type("INSTALLED-AT", device, site);
        LocallyDeployedNeo4JResponseProcessor processor(*context);
        const auto response = [](const json &row) {
            return json{{"results", json::array({{{"data", json::array({{{"row", json::array({row})}}})}}})}};
        };
        const auto node_result = processor.process(response({{"native_id", 1},
                                                             {"uuid", 281474976710657ULL},
                                                             {"labels", {"POLYGRAPHVertex", "Device_unit"}},
                                                             {"properties", json::object()}}),
                                                   ResultParseType::Node);
        ASSERT_EQ(node_result->data.size(), 1);
        EXPECT_EQ(node_result->data[0]->fields[0].as<Node>().type, device);
        const auto proxy_result = processor.process(response({{"uuid", 562949953421314ULL},
                                                              {"is_proxy", true},
                                                              {"remote-type", "Site/office"},
                                                              {"labels", {"Proxy"}},
                                                              {"properties", json::object()}}),
                                                    ResultParseType::Node);
        EXPECT_EQ(proxy_result->data[0]->fields[0].as<Node>().type, site);
        const auto edge_result = processor.process(response({{"uuid", 281474976710756ULL},
                                                             {"type", "INSTALLED_AT"},
                                                             {"start_native_id", 1},
                                                             {"end_native_id", 2},
                                                             {"properties", json::object()}}),
                                                   ResultParseType::Relationship);
        EXPECT_EQ(edge_result->data[0]->fields[0].as<Edge>().type, edge);
    }
} // namespace polygraph
