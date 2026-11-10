#pragma once
#include "ingestion/Import.hpp"

namespace polygraph::ingestion {
    // Recover only identity publication after a verified relational data commit.
    // Never reimports or overwrites application rows.
    [[nodiscard]] Json recover_postgres_relational_identity(const fs::path &manifest, const fs::path &target_config);
} // namespace polygraph::ingestion
