#include "RelationalCatalogInternal.hpp"

#include <algorithm>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>

namespace polygraph::postgres {
    namespace {
        std::string digest(std::istream &input) {
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
            if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
                detail::fail("SHA-256 initialization failed");
            std::array<char, 65536> buffer{};
            while (input) {
                input.read(buffer.data(), buffer.size());
                if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(input.gcount())) != 1)
                    detail::fail("SHA-256 update failed");
            }
            if (!input.eof())
                detail::fail("catalog hashing read failed");
            std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
            unsigned length{};
            if (EVP_DigestFinal_ex(context.get(), bytes.data(), &length) != 1)
                detail::fail("SHA-256 finalization failed");
            std::ostringstream output;
            for (unsigned i = 0; i < length; ++i)
                output << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(bytes[i]);
            return output.str();
        }
        json property_contract(const std::vector<PropertyMapping> &properties) {
            json result = json::object();
            for (const auto &p: properties)
                result[p.property] = {{"column", p.column}, {"type", static_cast<int>(p.type)}};
            return result;
        }
        BackendId checked_owner(GlobalId id, const std::string &name) {
            const auto owner = config::backend_id_from_string(name);
            if (insertion::BackendEncodedId::canonical_id(id) != id ||
                insertion::BackendEncodedId::decode(id, detail::routing) != owner)
                detail::fail("catalog owner/ID mismatch");
            return owner;
        }
    } // namespace
    namespace detail {
        std::string hash_text(std::string_view text) {
            std::istringstream input{std::string(text)};
            return digest(input);
        }
        std::string hash_file(const std::filesystem::path &path) {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                fail("cannot hash catalog '" + path.string() + "'");
            return digest(input);
        }
        CatalogFile read_catalog_file(const std::filesystem::path &path, bool relational) {
            std::ifstream input(path);
            if (!input)
                fail("cannot open manifest '" + path.string() + "'");
            const auto m = json::parse(input);
            if (m.at("schema_version") != 1 || m.at("engine_version") != 1 || m.at("state") != "prepared" ||
                m.at("id_strategy") != "typed-external-v1")
                fail("expected prepared typed source catalog");
            if (relational &&
                (m.value("format", std::string{}) != "polygraph.relational-identity" || m.at("version") != 1))
                fail("expected relational identity generation");
            const auto root = std::filesystem::canonical(path).parent_path();
            const std::filesystem::path relative = m.at("index").at("path").get<std::string>();
            if (relative.empty() || relative.is_absolute())
                fail("catalog index path must be relative");
            const auto index = std::filesystem::canonical(root / relative);
            const auto confined = index.lexically_relative(root);
            if (confined.empty() || *confined.begin() == ".." || !std::filesystem::is_regular_file(index))
                fail("catalog index escapes generation directory");
            if (std::filesystem::file_size(index) != m.at("index").at("bytes").get<std::uintmax_t>() ||
                hash_file(index) != m.at("index").at("sha256"))
                fail("catalog size/checksum mismatch");
            if (relational && m.at("generation_id") != m.at("index").at("sha256"))
                fail("catalog generation mismatch");
            return {m, index};
        }
    } // namespace detail
    domain::SourceIdentity relational_source_identity(const NodeMapping &node, const json &value) {
        domain::SourceIdentity result{node.name_space, node.logical_type, value};
        if (node.key_codec == KeyCodec::int64) {
            if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > INT64_MAX))
                detail::fail("node '" + node.logical_type + "' requires an exact signed SQL integer key");
            result.value = value.get<std::int64_t>();
        } else {
            if (!value.is_string())
                detail::fail("node '" + node.logical_type + "' requires a string key");
            auto text = value.get<std::string>();
            if (text.find('\0') != std::string::npos)
                detail::fail("SQL string key cannot contain NUL");
            if (node.key_codec == KeyCodec::uuid) {
                if (text.size() != 36)
                    detail::fail("UUID key requires canonical 36-character syntax");
                for (std::size_t i = 0; i < text.size(); ++i) {
                    auto &c = text[i];
                    if (i == 8 || i == 13 || i == 18 || i == 23) {
                        if (c != '-')
                            detail::fail("invalid UUID key");
                    } else if (c >= 'A' && c <= 'F')
                        c = static_cast<char>(c - 'A' + 'a');
                    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                        detail::fail("invalid UUID key");
                }
            }
            result.value = std::move(text);
        }
        (void) domain::encode_source_identity(result);
        return result;
    }
    std::string relational_mapping_fingerprint(const RelationalMapping &mapping) {
        json nodes = json::object(), relationships = json::object();
        for (const auto &n: mapping.nodes())
            nodes[n.logical_type] = {{"schema", n.table.schema},
                                     {"table", n.table.name},
                                     {"key", n.primary_key},
                                     {"namespace", n.name_space},
                                     {"codec", static_cast<int>(n.key_codec)},
                                     {"properties", property_contract(n.properties)}};
        for (const auto &r: mapping.relationships())
            relationships[r.logical_type] = {
                    {"kind", static_cast<int>(r.kind)},
                    {"from", mapping.node(r.from_type).logical_type},
                    {"to", mapping.node(r.to_type).logical_type},
                    {"schema", r.table.schema},
                    {"table", r.table.name},
                    {"fromColumn", r.from_column},
                    {"toColumn", r.to_column},
                    {"primaryKey", r.primary_key ? json(*r.primary_key) : json(nullptr)},
                    {"codec", r.key_codec ? json(static_cast<int>(*r.key_codec)) : json(nullptr)},
                    {"properties", property_contract(r.properties)},
                    {"owner", static_cast<int>(r.owner)},
                    {"column", r.column}};
        return detail::hash_text(json{{"version", 1}, {"nodes", nodes}, {"relationships", relationships}}.dump());
    }
    RelationalIdentityResolver::RelationalIdentityResolver(const std::filesystem::path &manifest,
                                                           RelationalMapping mapping,
                                                           std::optional<std::string> expected_generation) :
        mapping_(std::move(mapping)) {
        auto file = detail::read_catalog_file(manifest, true);
        const auto fingerprint = relational_mapping_fingerprint(mapping_);
        if (file.manifest.at("mapping_sha256") != fingerprint)
            detail::fail("catalog was produced for a different mapping");
        generation_ = file.manifest.at("generation_id").get<std::string>();
        if (expected_generation && *expected_generation != generation_)
            detail::fail("unexpected catalog generation");
        index_ = std::move(file.index);
        const auto db = detail::open(index_, true);
        const auto metadata = detail::statement(db.get(), "SELECT value FROM metadata WHERE key='mapping_sha256'");
        if (sqlite3_step(metadata.get()) != SQLITE_ROW || detail::text(metadata.get(), 0) != fingerprint ||
            sqlite3_step(metadata.get()) != SQLITE_DONE)
            detail::fail("catalog metadata disagrees with mapping");
    }
    std::vector<ResolvedRelationalNode>
    RelationalIdentityResolver::resolve(std::span<const domain::SourceIdentity> sources) const {
        if (sources.size() > max_batch_size)
            detail::fail("identity lookup batch exceeds 4096 vertices");
        const auto db = detail::open(index_, true);
        const auto lookup = detail::statement(db.get(), "SELECT id,type,owner FROM nodes WHERE key=?");
        std::vector<ResolvedRelationalNode> result;
        result.reserve(sources.size());
        for (const auto &source: sources) {
            const auto &n = detail::node_by_name(mapping_, source.type);
            if (source.name_space != n.name_space)
                detail::fail("source namespace mismatch for '" + source.type + "'");
            auto canonical = relational_source_identity(n, source.value);
            detail::bind(lookup.get(), 1, domain::encode_source_identity(canonical));
            if (sqlite3_step(lookup.get()) != SQLITE_ROW)
                detail::fail("missing or retired source identity for '" + source.type + "'");
            const auto id = detail::id(lookup.get(), 0);
            if (detail::text(lookup.get(), 1) != n.logical_type)
                detail::fail("catalog type/identity mismatch");
            const auto owner = checked_owner(id, detail::text(lookup.get(), 2));
            if (sqlite3_step(lookup.get()) != SQLITE_DONE)
                detail::fail("duplicate/error in source lookup");
            result.push_back({{id, n.type}, owner, std::move(canonical)});
            sqlite3_reset(lookup.get());
            sqlite3_clear_bindings(lookup.get());
        }
        return result;
    }
    std::vector<ResolvedRelationalNode> RelationalIdentityResolver::reverse(std::span<const NodeRef> nodes) const {
        if (nodes.size() > max_batch_size)
            detail::fail("identity lookup batch exceeds 4096 vertices");
        const auto db = detail::open(index_, true);
        const auto lookup = detail::statement(db.get(), "SELECT key,type,owner FROM nodes WHERE id=?");
        std::vector<ResolvedRelationalNode> result;
        result.reserve(nodes.size());
        for (const auto &node: nodes) {
            detail::bind(lookup.get(), 1, global_id_to_signed_storage(node.global_id));
            if (sqlite3_step(lookup.get()) != SQLITE_ROW)
                detail::fail("missing or retired internal vertex ID");
            const auto key = detail::text(lookup.get(), 0);
            const auto &n = detail::node_by_name(mapping_, detail::text(lookup.get(), 1));
            const auto source = detail::decode_key(key);
            const auto canonical = relational_source_identity(n, source.value);
            if (source.name_space != n.name_space || domain::encode_source_identity(canonical) != key ||
                (node.type != unresolved_type_id && node.type != n.type))
                detail::fail("catalog key/type mismatch");
            const auto owner = checked_owner(node.global_id, detail::text(lookup.get(), 2));
            if (sqlite3_step(lookup.get()) != SQLITE_DONE)
                detail::fail("duplicate/error in reverse lookup");
            result.push_back({{node.global_id, n.type}, owner, canonical});
            sqlite3_reset(lookup.get());
            sqlite3_clear_bindings(lookup.get());
        }
        return result;
    }
} // namespace polygraph::postgres
