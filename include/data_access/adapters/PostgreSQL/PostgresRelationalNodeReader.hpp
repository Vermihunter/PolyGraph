#pragma once

#include <libpq-fe.h>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"
#include "data_model/Record.hpp"

namespace polygraph::postgres {
    class PostgresRelationalNodeReader {
    public:
        PostgresRelationalNodeReader(RelationalMapping mapping, const RelationalIdentityResolver &identities);
        // One Record with one Node Value per requested NodeRef, in request order.
        // Duplicate requests produce independent records. At most 4096 per call.
        // Requires an idle caller-owned connection; result publication is all-or-nothing.
        [[nodiscard]] std::vector<record_t> read_nodes(PGconn *, std::span<const NodeRef>) const;

    private:
        RelationalMapping mapping_;
        const RelationalIdentityResolver &identities_;
    };
} // namespace polygraph::postgres
