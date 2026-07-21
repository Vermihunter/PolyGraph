/**
 * @file src/insertion/BackendEncodedUuid.cpp
 * @brief Implementation of compact backend routing in 64-bit ID MSBs.
 */

#include "insertion/BackendEncodedUuid.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>

namespace cpgdb::insertion {
    namespace {

        [[nodiscard]] std::size_t backend_code(const BackendId backend_id,
                                               const std::span<const BackendId> routing_backends) {
            const auto iterator = std::ranges::find(routing_backends, backend_id);
            if (iterator == routing_backends.end()) {
                throw std::invalid_argument("Backend is not present in the ID routing layout");
            }
            return static_cast<std::size_t>(std::distance(routing_backends.begin(), iterator));
        }

        void validate_layout(const std::span<const BackendId> routing_backends) {
            if (routing_backends.empty()) {
                throw std::invalid_argument("ID routing layout must contain at least one backend");
            }
            for (std::size_t i = 0; i < routing_backends.size(); ++i) {
                if (std::ranges::find(routing_backends.subspan(i + 1), routing_backends[i]) != routing_backends.end()) {
                    throw std::invalid_argument("ID routing layout contains duplicate backend IDs");
                }
            }
        }

        [[nodiscard]] GlobalId random_payload() {
            thread_local std::mt19937_64 generator{std::random_device{}()};
            return generator();
        }

    } // namespace

    std::size_t BackendEncodedId::bit_count(const std::size_t backend_count) {
        if (backend_count == 0) {
            throw std::invalid_argument("ID routing layout must contain at least one backend");
        }
        return backend_count == 1 ? 0U : std::bit_width(backend_count - 1U);
    }

    std::size_t BackendEncodedId::payload_bit_count(const std::size_t backend_count) {
        return 64U - bit_count(backend_count);
    }

    GlobalId BackendEncodedId::payload_mask(const std::size_t backend_count) {
        const std::size_t payload_bits = payload_bit_count(backend_count);
        return payload_bits == 64U ? (std::numeric_limits<GlobalId>::max)() : (GlobalId{1} << payload_bits) - 1U;
    }

    GlobalId BackendEncodedId::generate(const BackendId backend_id, const std::span<const BackendId> routing_backends) {
        return from_local_id(random_payload() & payload_mask(routing_backends.size()), backend_id, routing_backends);
    }

    GlobalId BackendEncodedId::encode(const GlobalId id, const BackendId backend_id,
                                      const std::span<const BackendId> routing_backends) {
        return from_local_id(local_id(id, routing_backends), backend_id, routing_backends);
    }

    GlobalId BackendEncodedId::from_local_id(const GlobalId local_id_value, const BackendId backend_id,
                                             const std::span<const BackendId> routing_backends) {
        validate_layout(routing_backends);
        const std::size_t payload_bits = payload_bit_count(routing_backends.size());
        const GlobalId mask = payload_mask(routing_backends.size());
        if ((local_id_value & ~mask) != 0U) {
            throw std::overflow_error("Backend-local ID does not fit in the configured global-ID payload");
        }

        if (payload_bits == 64U) {
            return local_id_value;
        }
        const auto code = static_cast<GlobalId>(backend_code(backend_id, routing_backends));
        return (code << payload_bits) | local_id_value;
    }

    GlobalId BackendEncodedId::local_id(const GlobalId id, const std::span<const BackendId> routing_backends) {
        validate_layout(routing_backends);
        return id & payload_mask(routing_backends.size());
    }

    BackendId BackendEncodedId::decode(const GlobalId id, const std::span<const BackendId> routing_backends) {
        validate_layout(routing_backends);
        const std::size_t payload_bits = payload_bit_count(routing_backends.size());
        const std::size_t code = payload_bits == 64U ? 0U : static_cast<std::size_t>(id >> payload_bits);
        if (code >= routing_backends.size()) {
            throw std::invalid_argument("Global ID contains an unused backend routing code");
        }
        return routing_backends[code];
    }

    std::optional<BackendId> BackendEncodedId::try_decode(const GlobalId id,
                                                          const std::span<const BackendId> routing_backends) noexcept {
        try {
            return decode(id, routing_backends);
        } catch (...) {
            return std::nullopt;
        }
    }

} // namespace cpgdb::insertion
