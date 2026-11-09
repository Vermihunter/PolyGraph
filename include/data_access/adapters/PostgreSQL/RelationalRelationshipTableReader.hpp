#pragma once

#include <functional>
#include <libpq-fe.h>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"

namespace polygraph::postgres {
    enum class RelationshipLookupDirection { outgoing, incoming, both };
    struct RelationalRelationshipLookup {
        // Empty lists select all mapped table relationships / all neighbor types.
        std::vector<TypeId> relationship_types = {};
        std::vector<TypeId> target_types = {};
        RelationshipLookupDirection direction = RelationshipLookupDirection::both;
        std::size_t batch_size = 1024;
    };
    struct RelationalRelationshipRow {
        NodeRef origin;
        TypeId type;
        ResolvedRelationalNode from;
        ResolvedRelationalNode to;
        bool reverse;
        // Native application edge PK, if mapped. This is not a canonical edge ID.
        std::optional<json> external_key;
        json properties;
    };
    class RelationalRelationshipTableReader {
    public:
        using Consumer = std::function<void(std::span<const RelationalRelationshipRow>)>;
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
