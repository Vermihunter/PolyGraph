#ifndef POLYGRAPH_INGESTION_GRAPH_MANIFEST_HPP
#define POLYGRAPH_INGESTION_GRAPH_MANIFEST_HPP

#include "ingestion/InputAdapterRegistry.hpp"

namespace polygraph::ingestion {
    struct GraphManifest {
        std::string id;
        fs::path path;
        fs::path schema;
        fs::path runtime_data;
        fs::path backend_config;
        fs::path target_config;
        std::string identity_strategy;
        PrepareOptions options;
    };

    // Additive native ingestion configuration in polygraph.domain version 1.
    // The selected registry must outlive this value and any preparation using its options.
    [[nodiscard]] GraphManifest load_graph_manifest(const fs::path &path,
                                                    const InputAdapterRegistry *registry = nullptr);
    // Read-only configuration/inventory preflight: never creates files, connects or executes commands.
    [[nodiscard]] Json validate_graph_manifest(const GraphManifest &graph, bool execution = false);
    [[nodiscard]] Json native_executor_capabilities();
} // namespace polygraph::ingestion
#endif
