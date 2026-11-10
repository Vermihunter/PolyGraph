#include "data_access/adapters/PostgreSQL/RelationalForeignKeyReader.hpp"
#include "RelationalReaderMapping.hpp"
#include "RelationalRelationshipReadInternal.hpp"

namespace polygraph::postgres {
    RelationalForeignKeyReader::RelationalForeignKeyReader(RelationalMapping mapping,
                                                           const RelationalIdentityResolver &identities) :
        mapping_(std::move(mapping)), identities_(identities) {
        detail::validate_read_mapping(mapping_, identities_);
    }
    void RelationalForeignKeyReader::stream(PGconn *connection, std::span<const NodeRef> frontier,
                                            const RelationalRelationshipLookup &lookup, const Consumer &consume) const {
        detail::stream_relationships(connection, frontier, lookup, consume, mapping_, identities_,
                                     RelationshipKind::foreign_key);
    }
} // namespace polygraph::postgres
