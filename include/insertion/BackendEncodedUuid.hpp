/**
 * @file include/insertion/BackendEncodedUuid.hpp
 * @brief Encodes authoritative placement plus cache residency in uint64 MSBs.
 */
#ifndef POLYGRAPH_INSERTION_BACKEND_ENCODED_UUID_HPP
#define POLYGRAPH_INSERTION_BACKEND_ENCODED_UUID_HPP

#include "data_model/EntityId.hpp"
#include "infrastructure/BackendId.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace polygraph::insertion {

    /**
     * GlobalId v2 layout.
     *
     * The high LOCATION_BITS bits are a bitmask. One bit identifies the authoritative
     * owner (Neo4j/Arango/Memgraph, or a GPU device for legacy GPU-primary objects),
     * and additional bits identify cache/execution locations (Redis, mmap, GPU N).
     * The lower PAYLOAD_BITS bits are the stable backend-local payload.
     */
    class BackendEncodedId {
    public:
        using LocationMask = std::uint32_t;

        static constexpr std::size_t LOCATION_BITS = 16U;
        static constexpr std::size_t PAYLOAD_BITS = 64U - LOCATION_BITS;
        static constexpr LocationMask NEO4J_LOCATION = LocationMask{1} << 0U;
        static constexpr LocationMask ARANGO_LOCATION = LocationMask{1} << 1U;
        static constexpr LocationMask MEMGRAPH_LOCATION = LocationMask{1} << 2U;
        static constexpr LocationMask REDIS_LOCATION = LocationMask{1} << 3U;
        static constexpr LocationMask MMAP_LOCATION = LocationMask{1} << 4U;
        static constexpr unsigned int GPU_BASE_BIT = 5U;
        // Bit 15 is reserved for PostgreSQL ownership. GPUs therefore occupy bits 5..14.
        static constexpr unsigned int POSTGRESQL_BIT = 15U;
        static constexpr LocationMask POSTGRESQL_LOCATION = LocationMask{1} << POSTGRESQL_BIT;
        static constexpr unsigned int MAX_GPU_DEVICES = POSTGRESQL_BIT - GPU_BASE_BIT;

        [[nodiscard]] static std::size_t bit_count(std::size_t backend_count);
        [[nodiscard]] static std::size_t payload_bit_count(std::size_t backend_count);
        [[nodiscard]] static GlobalId payload_mask(std::size_t backend_count);
        [[nodiscard]] static constexpr GlobalId payload_mask() noexcept { return (GlobalId{1} << PAYLOAD_BITS) - 1U; }

        [[nodiscard]] static LocationMask gpu_location_bit(unsigned int device_id);
        [[nodiscard]] static constexpr LocationMask gpu_location_mask() noexcept {
            return ((LocationMask{1} << MAX_GPU_DEVICES) - 1U) << GPU_BASE_BIT;
        }
        [[nodiscard]] static constexpr LocationMask persistent_owner_mask() noexcept {
            return NEO4J_LOCATION | ARANGO_LOCATION | MEMGRAPH_LOCATION | POSTGRESQL_LOCATION;
        }
        [[nodiscard]] static constexpr LocationMask cache_location_mask() noexcept {
            return REDIS_LOCATION | MMAP_LOCATION | gpu_location_mask();
        }

        [[nodiscard]] static GlobalId generate(BackendId backend_id, std::span<const BackendId> routing_backends);
        [[nodiscard]] static GlobalId encode(GlobalId id, BackendId backend_id,
                                             std::span<const BackendId> routing_backends);
        [[nodiscard]] static GlobalId from_local_id(GlobalId local_id, BackendId backend_id,
                                                    std::span<const BackendId> routing_backends,
                                                    LocationMask cache_residency = 0U);
        [[nodiscard]] static GlobalId local_id(GlobalId id, std::span<const BackendId> routing_backends);
        [[nodiscard]] static LocationMask location_mask(GlobalId id) noexcept;
        [[nodiscard]] static LocationMask owner_location(GlobalId id);
        [[nodiscard]] static LocationMask cache_residency(GlobalId id);
        [[nodiscard]] static GlobalId canonical_id(GlobalId id);
        [[nodiscard]] static GlobalId with_cache_residency(GlobalId id, LocationMask cache_residency);
        [[nodiscard]] static bool has_location(GlobalId id, LocationMask location) noexcept;

        [[nodiscard]] static BackendId decode(GlobalId id, std::span<const BackendId> routing_backends);
        [[nodiscard]] static std::optional<BackendId> try_decode(GlobalId id,
                                                                 std::span<const BackendId> routing_backends) noexcept;
    };

    using BackendEncodedUuid = BackendEncodedId;

} // namespace polygraph::insertion
#endif // POLYGRAPH_INSERTION_BACKEND_ENCODED_UUID_HPP
