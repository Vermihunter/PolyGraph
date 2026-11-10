#pragma once

#include <libpq-fe.h>
#include "data_access/adapters/PostgreSQL/RelationalRelationshipRead.hpp"

namespace polygraph::postgres::detail {
    void stream_relationships(PGconn *, std::span<const NodeRef>, const RelationalRelationshipLookup &,
                              const RelationalRelationshipConsumer &, const RelationalMapping &,
                              const RelationalIdentityResolver &, RelationshipKind);
} // namespace polygraph::postgres::detail
