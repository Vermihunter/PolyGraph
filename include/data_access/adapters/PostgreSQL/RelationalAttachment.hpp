#pragma once

#include <libpq-fe.h>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"

namespace polygraph::postgres {
    struct RelationalAttachmentOptions {
        std::filesystem::path output_dir;
        std::optional<std::filesystem::path> previous_manifest = {};
        // Adopt exact IDs/owners from a prepared typed-external-v1 ingestion index.
        std::optional<std::filesystem::path> source_manifest = {};
        std::size_t batch_size = 4096;
    };
    // Requires an idle connection; owns a repeatable-read, read-only snapshot.
    // Creates an immutable disk catalog; never changes application data/schema.
    [[nodiscard]] std::filesystem::path attach_relational_nodes(PGconn *, const RelationalMapping &,
                                                                const RelationalAttachmentOptions &);
} // namespace polygraph::postgres
