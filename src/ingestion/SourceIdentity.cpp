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
        if (identity.name_space.empty() || identity.type.empty() ||
            identity.name_space.find('\0') != std::string::npos || identity.type.find('\0') != std::string::npos)
            throw Error("Source identity requires nonempty NUL-free namespace and logical type");
        const auto &value = identity.value;
        const auto kind = value.is_string()            ? "string"
                          : value.is_number_unsigned() ? "uint64"
                          : value.is_number_integer()  ? "int64"
                                                       : "invalid";
        if (std::string_view(kind) == "invalid")
            throw Error("Source identity ID must be a string, int64 or uint64 for " + identity.type);
        try {
            return Json::array({"polygraph.source.v1", identity.name_space, edge ? "edge" : "vertex", identity.type,
                                kind, value})
                    .dump();
        } catch (const Json::exception &e) {
            throw Error("Invalid source identity encoding: " + std::string(e.what()));
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
