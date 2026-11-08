#include <gtest/gtest.h>
#include "data_access/adapters/StorageTypeNames.hpp"

namespace polygraph {
    TEST(StorageTypeNamesTest, MapsUnfamiliarLogicalNamesAndPhysicalResponses) {
        domain::LogicalGraphSchema schema;
        const auto device = schema.add_node_type("Device-unit");
        const auto sensor = schema.add_node_type("Sensorµ");
        const auto edge = schema.add_edge_type("REPORTS:TO", device, sensor);
        const StorageTypeNames names(schema);
        EXPECT_EQ(names.node_name(device), "Device_unit");
        EXPECT_EQ(names.node_name(sensor), "Sensor_");
        EXPECT_EQ(names.edge_name(edge), "REPORTS_TO");
        EXPECT_EQ(names.node_type("Device_unit"), device);
        EXPECT_EQ(names.node_type("Device-unit"), device);
        EXPECT_EQ(names.edge_type("REPORTS_TO"), edge);
        EXPECT_EQ(names.edge_type("REPORTS:TO"), edge);
    }
    TEST(StorageTypeNamesTest, RejectsLossyNameCollisions) {
        domain::LogicalGraphSchema schema;
        schema.add_node_type("Device-unit");
        schema.add_node_type("Device_unit");
        EXPECT_THROW(StorageTypeNames{schema}, std::invalid_argument);
    }
    TEST(StorageTypeNamesTest, SharedCollectionNamespaceIsBackendSpecific) {
        domain::LogicalGraphSchema schema;
        const auto node = schema.add_node_type("LINK");
        schema.add_edge_type("LINK", node, node);
        const StorageTypeNames names(schema);
        EXPECT_THROW(names.validate_shared_namespace(), std::invalid_argument);
    }
} // namespace polygraph
