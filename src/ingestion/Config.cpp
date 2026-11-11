#include <algorithm>
#include <charconv>
#include <set>
#include "Internal.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/StorageNames.hpp"

namespace polygraph::ingestion {
    using Codec = insertion::BackendEncodedId;
    namespace {
        constexpr std::array<BackendId, 3> routing{NEO4J, ARANGO, POSTGRESQL};
        BackendId backend_code(Backend backend) {
            switch (backend) {
                case Backend::neo4j:
                    return NEO4J;
                case Backend::arango:
                    return ARANGO;
                case Backend::postgresql:
                    return POSTGRESQL;
            }
            throw Error("Invalid backend");
        }
    } // namespace
    std::string backend_name(Backend backend) {
        switch (backend) {
            case Backend::neo4j:
                return "Neo4J";
            case Backend::arango:
                return "ArangoDB";
            case Backend::postgresql:
                return "PostgreSQL";
        }
        throw Error("Invalid backend");
    }
    Backend parse_backend(const std::string &name) {
        if (name == "Neo4J" || name == "Neo4j" || name == "NEO4J")
            return Backend::neo4j;
        if (name == "ArangoDB" || name == "Arango" || name == "ARANGO")
            return Backend::arango;
        if (name == "PostgreSQL" || name == "Postgres" || name == "POSTGRESQL")
            return Backend::postgresql;
        throw Error("Unsupported bulk ingestion backend: " + name);
    }
    Backend owner(Id id) {
        try {
            const auto code = Codec::decode(id, routing);
            if (code == NEO4J)
                return Backend::neo4j;
            if (code == ARANGO)
                return Backend::arango;
            if (code == POSTGRESQL)
                return Backend::postgresql;
        } catch (const std::exception &e) {
            throw Error("Invalid GlobalId " + std::to_string(id) + ": " + e.what());
        }
        throw Error("Unsupported GlobalId owner");
    }
    Id canonical_id(Id id) {
        (void) owner(id);
        return Codec::canonical_id(id);
    }
    Id make_id(Backend backend, Id payload) {
        try {
            return Codec::from_local_id(payload, backend_code(backend), routing);
        } catch (const std::exception &e) {
            throw Error(e.what());
        }
    }
    Id parse_id(const Json &value) {
        if (value.is_number_unsigned())
            return value.get<Id>();
        if (value.is_number_integer())
            return std::bit_cast<Id>(value.get<std::int64_t>());
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            Id id = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
            if (error == std::errc{} && end == text.data() + text.size() && !text.empty())
                return id;
            if (!text.empty() && text.front() == '-') {
                std::int64_t stored = 0;
                const auto [last, err] = std::from_chars(text.data(), text.data() + text.size(), stored);
                if (err == std::errc{} && last == text.data() + text.size())
                    return std::bit_cast<Id>(stored);
            }
        }
        throw Error("GlobalId must be an exact uint64 or signed BIGINT decimal: " + value.dump());
    }
    std::string sanitize_name(const std::string &name) {
        try {
            return storage_type_name(name);
        } catch (const std::invalid_argument &error) {
            throw Error(error.what());
        }
    }
    Placement load_placement(const fs::path &path) {
        Placement p{.configuration = read_json(path), .backends = {}};
        const auto &j = p.configuration;
        if (!j.is_object() || !j.contains("storageBackends") || !j["storageBackends"].is_array())
            throw Error("Placement requires storageBackends array");
        for (const auto &name: j["storageBackends"]) {
            const auto backend = parse_backend(name.get<std::string>());
            if (std::ranges::find(p.backends, backend) != p.backends.end())
                throw Error("Duplicate storage backend");
            p.backends.push_back(backend);
        }
        if (p.backends.empty())
            throw Error("storageBackends must not be empty");
        if (j.contains("strategy") && j["strategy"] != "auto" && j["strategy"] != "explicit" &&
            j["strategy"] != "schema")
            throw Error("Native ingestion currently requires schema/explicit placement");
        for (const auto *key: {"nodeSources", "relationshipSources"}) {
            if (j.contains(key)) {
                if (!j[key].is_object())
                    throw Error(std::string(key) + " must be an object");
                for (const auto &value: j[key])
                    p.validate(parse_backend(value.get<std::string>()));
            }
        }
        for (const auto *key: {"defaultNodeSource", "defaultRelationshipSource"})
            if (j.contains(key))
                p.validate(parse_backend(j[key].get<std::string>()));
        return p;
    }
    void Placement::validate(Backend backend) const {
        if (std::ranges::find(backends, backend) == backends.end())
            throw Error("Owner is absent from storageBackends: " + backend_name(backend));
    }
    Backend Placement::route(const Record &r, bool edge) const {
        const auto key = edge ? "relationshipSources" : "nodeSources";
        const auto fallback = edge ? "defaultRelationshipSource" : "defaultNodeSource";
        std::optional<Backend> configured;
        if (configuration.contains(key) && configuration[key].contains(r.type))
            configured = parse_backend(configuration[key][r.type].get<std::string>());
        else if (configuration.contains(fallback))
            configured = parse_backend(configuration[fallback].get<std::string>());
        if (r.id) {
            const auto encoded = owner(*r.id);
            validate(encoded);
            if (r.source && *r.source != encoded)
                throw Error("Record source disagrees with GlobalId: " + r.key);
            if (configured && *configured != encoded)
                throw Error("Placement would change canonical ID ownership: " + r.key);
            return encoded;
        }
        if (!configured)
            throw Error("No schema placement for " + std::string(edge ? "relationship " : "node ") + r.type);
        return *configured;
    }
} // namespace polygraph::ingestion

namespace polygraph::ingestion::detail {
    unsigned arango_import_tuning(const Json &target, const std::string &name, unsigned fallback, unsigned maximum) {
        if (!target.contains(name))
            return fallback;
        const auto &value = target.at(name);
        if (!value.is_number_integer() || value < 1 || value > maximum)
            throw Error("Arango " + name + " must be an integer in [1," + std::to_string(maximum) + "]");
        return value.get<unsigned>();
    }
    void validate_arango_import_options(const Json &target) {
        const auto batch = arango_import_tuning(target, "import_batch_mb", 1, 64);
        const auto buffer = arango_import_tuning(target, "import_buffer_mb", 128, 1024);
        (void) arango_import_tuning(target, "import_concurrency", 4, 32);
        if (buffer < batch)
            throw Error("Arango import_buffer_mb must be at least import_batch_mb");
    }
} // namespace polygraph::ingestion::detail
