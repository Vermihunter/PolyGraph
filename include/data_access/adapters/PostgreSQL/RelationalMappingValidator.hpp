#pragma once

#include <libpq-fe.h>
#include <unordered_map>
#include "data_access/adapters/PostgreSQL/RelationalMapping.hpp"

namespace polygraph::postgres {
    struct RelationalColumnMetadata {
        int number{};
        Oid type_oid{};
        int type_modifier{};
        bool nullable{};
        bool generated{};
        bool has_default{};
        std::string identity;
    };
    struct RelationalTableMetadata {
        SqlTable table;
        Oid oid{};
        std::unordered_map<std::string, RelationalColumnMetadata> columns;
    };
    // A startup validation result, not a schema lock. Revalidate after application DDL.
    class ValidatedRelationalMapping {
    public:
        [[nodiscard]] const RelationalMapping &mapping() const noexcept { return mapping_; }
        [[nodiscard]] const std::vector<RelationalTableMetadata> &tables() const noexcept { return tables_; }

    private:
        RelationalMapping mapping_;
        std::vector<RelationalTableMetadata> tables_;
        friend class RelationalMappingValidator;
    };
    class RelationalMappingValidator {
    public:
        // Read-only catalog/privilege inspection. Caller owns connection and transaction.
        [[nodiscard]] ValidatedRelationalMapping validate(PGconn *, const RelationalMapping &) const;
    };
} // namespace polygraph::postgres
