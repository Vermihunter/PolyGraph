#pragma once

#include <span>
#include "data_access/adapters/PostgreSQL/RelationalMapping.hpp"
#include "data_model/NodeRef.hpp"
#include "domain/graph/SourceIdentity.hpp"
#include "infrastructure/BackendId.hpp"

namespace polygraph::postgres {
    struct ResolvedRelationalNode {
        NodeRef node;
        BackendId owner;
        domain::SourceIdentity source;
    };
    [[nodiscard]] domain::SourceIdentity relational_source_identity(const NodeMapping &, const json &external_key);
    [[nodiscard]] std::string relational_mapping_fingerprint(const RelationalMapping &);

    // Immutable, checksummed disk catalog. Connections are per call; no shared SQLite handles.
    class RelationalIdentityResolver {
    public:
        static constexpr std::size_t max_batch_size = 4096;
        RelationalIdentityResolver(const std::filesystem::path &manifest, RelationalMapping mapping,
                                   std::optional<std::string> expected_generation = std::nullopt);
        [[nodiscard]] std::vector<ResolvedRelationalNode> resolve(std::span<const domain::SourceIdentity>) const;
        [[nodiscard]] std::vector<ResolvedRelationalNode> reverse(std::span<const NodeRef>) const;
        [[nodiscard]] const std::string &generation() const noexcept { return generation_; }
        [[nodiscard]] const RelationalMapping &mapping() const noexcept { return mapping_; }

    private:
        std::filesystem::path index_;
        RelationalMapping mapping_;
        std::string generation_;
    };
} // namespace polygraph::postgres
