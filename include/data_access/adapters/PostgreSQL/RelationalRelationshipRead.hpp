#pragma once

#include <functional>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"

namespace polygraph::postgres {
    enum class RelationshipLookupDirection { outgoing, incoming, both };
    struct RelationalRelationshipLookup {
        // Empty lists select all relationships supported by this reader / all neighbor types.
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
    using RelationalRelationshipConsumer = std::function<void(std::span<const RelationalRelationshipRow>)>;
} // namespace polygraph::postgres
