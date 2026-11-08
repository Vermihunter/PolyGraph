#pragma once

#include <filesystem>
#include "data_model/EntityId.hpp"
#include "domain/graph/SourceIdentity.hpp"
#include "infrastructure/BackendId.hpp"

namespace polygraph::cli {
    struct ResolvedSourceNode {
        GlobalId id;
        BackendId owner;
    };
    // Prepared artifacts must remain immutable while used by a runtime. No source
    // records or full identity dictionary are loaded; each lookup is SQLite read-only.
    class SourceNodeIdResolver {
    public:
        explicit SourceNodeIdResolver(const std::filesystem::path &prepared_manifest);
        [[nodiscard]] ResolvedSourceNode resolve(const domain::SourceIdentity &identity) const;

    private:
        std::filesystem::path index_;
    };
} // namespace polygraph::cli
