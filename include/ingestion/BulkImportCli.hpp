#ifndef POLYGRAPH_INGESTION_BULK_IMPORT_CLI_HPP
#define POLYGRAPH_INGESTION_BULK_IMPORT_CLI_HPP

#include "ingestion/InputAdapterRegistry.hpp"

namespace polygraph::ingestion {
    // Application composition hook. Registry must outlive the call. All commands,
    // including subprocess re-entry for Docker loading, use the application's registry.
    // load/verify-load replace the process with the existing lifecycle helper on success.
    int run_bulk_import_cli(int argc, char **argv, const InputAdapterRegistry &registry = default_input_adapters());
} // namespace polygraph::ingestion
#endif
