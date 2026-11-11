#pragma once

#include <libpq-fe.h>
#include "data_access/adapters/PostgreSQL/RelationalRelationshipRead.hpp"

namespace polygraph::postgres {
    class RelationalRelationshipTableReader {
    public:
        using Consumer = RelationalRelationshipConsumer;
        RelationalRelationshipTableReader(RelationalMapping mapping, const RelationalIdentityResolver &identities);
        // Idle caller-owned connection; owns a read-only snapshot. Frontier <=4096,
        // fetch <=1024. Prefixes delivered before an error must be discarded.
        void stream(PGconn *, std::span<const NodeRef> frontier, const RelationalRelationshipLookup &,
                    const Consumer &) const;

    private:
        RelationalMapping mapping_;
        const RelationalIdentityResolver &identities_;
    };
} // namespace polygraph::postgres
