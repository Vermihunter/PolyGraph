/**
 * @file src/insertion/BackendEncodedUuid.cpp
 * @brief Implementation for backend encoded uuid in the insertion planning module.
 */

#include "insertion/BackendEncodedUuid.hpp"

#include <boost/uuid/random_generator.hpp>
#include <cstdint>

namespace cpgdb::insertion {
    namespace {

        constexpr std::size_t backend_byte_count = sizeof(BackendId);
        static_assert(backend_byte_count == 4, "BackendEncodedUuid expects a 32-bit BackendId");

    } // namespace

    GlobalId BackendEncodedUuid::generate(const BackendId backend_id) {
        return encode(boost::uuids::random_generator{}(), backend_id);
    }

    GlobalId BackendEncodedUuid::encode(GlobalId uuid, const BackendId backend_id) {
        uuid.data[0] = static_cast<std::uint8_t>((backend_id >> 24U) & 0xFFU);
        uuid.data[1] = static_cast<std::uint8_t>((backend_id >> 16U) & 0xFFU);
        uuid.data[2] = static_cast<std::uint8_t>((backend_id >> 8U) & 0xFFU);
        uuid.data[3] = static_cast<std::uint8_t>(backend_id & 0xFFU);

        // Keep RFC 4122 version/variant bits valid after rewriting the ownership segment.
        uuid.data[6] = static_cast<std::uint8_t>((uuid.data[6] & 0x0FU) | 0x40U);
        uuid.data[8] = static_cast<std::uint8_t>((uuid.data[8] & 0x3FU) | 0x80U);
        return uuid;
    }

    BackendId BackendEncodedUuid::decode(const GlobalId &uuid) noexcept {
        return (static_cast<BackendId>(uuid.data[0]) << 24U) | (static_cast<BackendId>(uuid.data[1]) << 16U) |
               (static_cast<BackendId>(uuid.data[2]) << 8U) | static_cast<BackendId>(uuid.data[3]);
    }

    std::optional<BackendId> BackendEncodedUuid::decode_nonzero(const GlobalId &uuid) noexcept {
        const BackendId backend_id = decode(uuid);
        return backend_id == BackendId{} ? std::nullopt : std::optional<BackendId>{backend_id};
    }

} // namespace cpgdb::insertion
