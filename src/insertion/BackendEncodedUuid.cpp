/**
 * @file src/insertion/BackendEncodedUuid.cpp
 * @brief Implementation of compact backend routing in UUID suffix bits.
 */

#include "insertion/BackendEncodedUuid.hpp"

#include <algorithm>
#include <bit>
#include <boost/uuid/random_generator.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace cpgdb::insertion {
    namespace {

        [[nodiscard]] std::size_t backend_code(BackendId backend_id, std::span<const BackendId> routing_backends) {
            const auto iterator = std::ranges::find(routing_backends, backend_id);
            if (iterator == routing_backends.end()) {
                throw std::invalid_argument("Backend is not present in the UUID routing layout");
            }
            return static_cast<std::size_t>(std::distance(routing_backends.begin(), iterator));
        }

        void validate_layout(std::span<const BackendId> routing_backends) {
            if (routing_backends.empty()) {
                throw std::invalid_argument("UUID routing layout must contain at least one backend");
            }
            if (routing_backends.size() > (std::numeric_limits<std::uint32_t>::max)()) {
                throw std::invalid_argument("UUID routing layout is too large");
            }
            for (std::size_t i = 0; i < routing_backends.size(); ++i) {
                if (std::ranges::find(routing_backends.subspan(i + 1), routing_backends[i]) != routing_backends.end()) {
                    throw std::invalid_argument("UUID routing layout contains duplicate backend IDs");
                }
            }
        }

        void set_suffix_bit(GlobalId &uuid, const std::size_t suffix_bit, const bool value) noexcept {
            const std::size_t byte_index = 15U - (suffix_bit / 8U);
            const auto mask = static_cast<std::uint8_t>(1U << (suffix_bit % 8U));
            if (value) {
                uuid.data[byte_index] = static_cast<std::uint8_t>(uuid.data[byte_index] | mask);
            } else {
                uuid.data[byte_index] = static_cast<std::uint8_t>(uuid.data[byte_index] & ~mask);
            }
        }

        [[nodiscard]] bool suffix_bit(const GlobalId &uuid, const std::size_t bit) noexcept {
            const std::size_t byte_index = 15U - (bit / 8U);
            const auto mask = static_cast<std::uint8_t>(1U << (bit % 8U));
            return (uuid.data[byte_index] & mask) != 0U;
        }

    } // namespace

    std::size_t BackendEncodedUuid::bit_count(const std::size_t backend_count) {
        if (backend_count == 0) {
            throw std::invalid_argument("UUID routing layout must contain at least one backend");
        }
        return backend_count == 1 ? 0U : std::bit_width(backend_count - 1U);
    }

    GlobalId BackendEncodedUuid::generate(const BackendId backend_id,
                                          const std::span<const BackendId> routing_backends) {
        return encode(boost::uuids::random_generator{}(), backend_id, routing_backends);
    }

    GlobalId BackendEncodedUuid::encode(GlobalId uuid, const BackendId backend_id,
                                        const std::span<const BackendId> routing_backends) {
        validate_layout(routing_backends);
        const std::size_t bits = bit_count(routing_backends.size());
        if (bits > 128U) {
            throw std::invalid_argument("UUID routing layout requires more than 128 bits");
        }

        const std::size_t code = backend_code(backend_id, routing_backends);
        for (std::size_t bit = 0; bit < bits; ++bit) {
            set_suffix_bit(uuid, bit, ((code >> bit) & 1U) != 0U);
        }
        return uuid;
    }

    BackendId BackendEncodedUuid::decode(const GlobalId &uuid, const std::span<const BackendId> routing_backends) {
        validate_layout(routing_backends);
        const std::size_t bits = bit_count(routing_backends.size());
        if (bits > 128U) {
            throw std::invalid_argument("UUID routing layout requires more than 128 bits");
        }

        std::size_t code = 0;
        for (std::size_t bit = 0; bit < bits; ++bit) {
            if (suffix_bit(uuid, bit)) {
                code |= (std::size_t{1} << bit);
            }
        }
        if (code >= routing_backends.size()) {
            throw std::invalid_argument("UUID contains an unused backend routing code");
        }
        return routing_backends[code];
    }

    std::optional<BackendId>
    BackendEncodedUuid::try_decode(const GlobalId &uuid, const std::span<const BackendId> routing_backends) noexcept {
        try {
            return decode(uuid, routing_backends);
        } catch (...) {
            return std::nullopt;
        }
    }

} // namespace cpgdb::insertion
