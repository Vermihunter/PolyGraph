#pragma once

#include <libpq-fe.h>
#include "data_access/adapters/PostgreSQL/RelationalRelationshipRead.hpp"

namespace polygraph::postgres {
    class RelationalForeignKeyReader {
    public:
        using Consumer = RelationalRelationshipConsumer;
        RelationalForeignKeyReader(RelationalMapping mapping, const RelationalIdentityResolver &identities);
        // Same connection, lifetime, batching and callback contract as the table reader.
        void stream(PGconn *, std::span<const NodeRef>, const RelationalRelationshipLookup &, const Consumer &) const;

    private:
        RelationalMapping mapping_;
        const RelationalIdentityResolver &identities_;
    };
} // namespace polygraph::postgres
