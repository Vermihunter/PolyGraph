#include <gtest/gtest.h>
#include "ingestion/BulkImportCli.hpp"

using namespace polygraph::ingestion;
TEST(BulkImportCliTest, ApplicationSuppliedRegistryReachesSharedWorkflow) {
    auto registry = default_input_adapters();
    registry.add({.name = "example-proof",
                  .factory = [](const auto &, const auto &) { return std::unique_ptr<IGraphInputAdapter>{}; },
                  .input_files = [](const auto &) { return std::vector<fs::path>{}; }});
    char program[] = "example-import";
    char command[] = "adapters";
    char *argv[] = {program, command};
    testing::internal::CaptureStdout();
    const auto status = run_bulk_import_cli(2, argv, registry);
    const auto result = testing::internal::GetCapturedStdout();
    EXPECT_EQ(status, 0);
    EXPECT_EQ(result, "example-proof\nldbc-snb\npolygraph-jsonl\n");
    EXPECT_EQ(default_input_adapters().names().size(), 2);
}
