#include "Internal.hpp"

namespace polygraph::ingestion {
    std::string input_id_strategy(InputIdPolicy policy) {
        switch (policy) {
            case InputIdPolicy::canonical:
                return "preserve-canonical-v2";
            case InputIdPolicy::ldbc_block31:
                return "ldbc-block31-v3";
            case InputIdPolicy::typed_external:
                return "typed-external-v1";
        }
        throw Error("Unsupported input ID policy");
    }
    std::string source_identity_key(const SourceIdentity &identity, bool edge) {
        try {
            return domain::encode_source_identity(identity, edge);
        } catch (const std::exception &e) {
            throw Error(e.what());
        }
    }
} // namespace polygraph::ingestion
namespace polygraph::ingestion::detail {
    void resolve_record_keys(Record &record, InputIdPolicy policy, bool edge) {
        if (policy != InputIdPolicy::typed_external) {
            if (record.identity || record.from_identity || record.to_identity)
                throw Error("Structured source identities require typed-external-v1 policy on " + record.type);
            return;
        }
        if (!record.key.empty() || !record.from.empty() || !record.to.empty())
            throw Error("Typed source records must use structured identity/endpoints, not opaque keys: " + record.type);
        if (!edge && !record.identity)
            throw Error("Missing typed vertex identity on " + record.type);
        if (record.identity) {
            if (record.identity->type != record.type)
                throw Error("Source identity type differs from logical record type: " + record.type);
            record.key = source_identity_key(*record.identity, edge);
        }
        if (edge) {
            if (!record.from_identity || !record.to_identity)
                throw Error("Missing typed relationship endpoint on " + record.type);
            record.from = source_identity_key(*record.from_identity);
            record.to = source_identity_key(*record.to_identity);
        } else if (record.from_identity || record.to_identity) {
            throw Error("Vertex cannot declare relationship endpoints: " + record.type);
        }
    }
} // namespace polygraph::ingestion::detail
