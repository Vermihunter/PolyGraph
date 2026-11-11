#pragma once

#include <algorithm>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"

namespace polygraph::postgres::detail {
    inline void validate_read_mapping(const RelationalMapping &mapping_,
                                      const RelationalIdentityResolver &identities_) {
        if (relational_mapping_fingerprint(mapping_) != relational_mapping_fingerprint(identities_.mapping()))
            throw std::invalid_argument("Relational reader/catalog mapping mismatch");
        for (const auto &n: mapping_.nodes()) {
            const auto it =
                    std::ranges::find(identities_.mapping().nodes(), n.logical_type, &NodeMapping::logical_type);
            if (it == identities_.mapping().nodes().end() || it->type != n.type)
                throw std::invalid_argument("Relational reader and resolver require the same logical schema type IDs");
        }
    }
} // namespace polygraph::postgres::detail
