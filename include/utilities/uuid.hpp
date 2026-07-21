/**
 * @file include/utilities/uuid.hpp
 * @brief Compatibility helpers for the project's numeric global identifiers.
 *
 * The filename is retained to avoid a wide include-path migration. The project
 * no longer depends on Boost.UUID; every identifier is a std::uint64_t.
 */

#ifndef CPGDB_UUID_HPP
#define CPGDB_UUID_HPP

#include "data_model/EntityId.hpp"

#include <bit>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace cpgdb {

// Backwards-compatible type name for older call sites.
using uuid = GlobalId;

[[nodiscard]] inline std::string global_id_to_string(const GlobalId id) {
    return std::to_string(id);
}

[[nodiscard]] inline GlobalId global_id_from_string(const std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("Global ID must not be empty");
    }

    GlobalId value{};
    const char *first = text.data();
    const char *last = text.data() + text.size();
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        first += 2;
        base = 16;
    }

    const auto [end, error] = std::from_chars(first, last, value, base);
    if (error != std::errc{} || end != last) {
        throw std::invalid_argument("Invalid 64-bit global ID: " + std::string(text));
    }
    return value;
}

/** Preserve all 64 bits when storing an ID in a signed database INTEGER. */
[[nodiscard]] inline std::int64_t global_id_to_signed_storage(const GlobalId id) noexcept {
    return std::bit_cast<std::int64_t>(id);
}

/** Restore the unsigned bit pattern returned by a signed database INTEGER. */
[[nodiscard]] inline GlobalId global_id_from_signed_storage(const std::int64_t id) noexcept {
    return std::bit_cast<GlobalId>(id);
}

} // namespace cpgdb

#endif // CPGDB_UUID_HPP
