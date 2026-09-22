/**
 * @file src/insertion/BackendEncodedUuid.cpp
 * @brief Fixed-width location/residency bitmask encoding for GlobalId.
 */
#include "insertion/BackendEncodedUuid.hpp"
#include "utilities/BackendIds.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>

namespace polygraph::insertion {
    namespace {

        void validate_layout(const std::span<const BackendId> routing_backends) {
            if (routing_backends.empty()) {
                throw std::invalid_argument("ID routing layout must contain at least one authoritative backend");
            }
            for (std::size_t i = 0; i < routing_backends.size(); ++i) {
                if (std::ranges::find(routing_backends.subspan(i + 1), routing_backends[i]) != routing_backends.end()) {
                    throw std::invalid_argument("ID routing layout contains duplicate backend IDs");
                }
            }
        }

        [[nodiscard]] bool configured_owner(const BackendId owner, const std::span<const BackendId> routing_backends) {
            if (is_gpu_backend_id(owner)) {
                return std::ranges::any_of(routing_backends, [](const BackendId id) { return is_gpu_backend_id(id); });
            }
            return std::ranges::find(routing_backends, owner) != routing_backends.end();
        }

        [[nodiscard]] BackendEncodedId::LocationMask owner_mask_for_backend(const BackendId backend_id) {
            if (backend_id == NEO4J)
                return BackendEncodedId::NEO4J_LOCATION;
            if (backend_id == ARANGO)
                return BackendEncodedId::ARANGO_LOCATION;
            if (backend_id == MEMGRAPH)
                return BackendEncodedId::MEMGRAPH_LOCATION;
            if (backend_id == POSTGRESQL)
                return BackendEncodedId::POSTGRESQL_LOCATION;
            if (is_gpu_backend_id(backend_id)) {
                return BackendEncodedId::gpu_location_bit(gpu_device_id_from_backend(backend_id));
            }
            throw std::invalid_argument("Backend cannot own a GlobalId");
        }

        [[nodiscard]] GlobalId random_payload() {
            thread_local std::mt19937_64 generator{std::random_device{}()};
            return generator() & BackendEncodedId::payload_mask();
        }

    } // namespace

    std::size_t BackendEncodedId::bit_count(const std::size_t backend_count) {
        if (backend_count == 0)
            throw std::invalid_argument("ID routing layout must contain at least one backend");
        return LOCATION_BITS;
    }

    std::size_t BackendEncodedId::payload_bit_count(const std::size_t backend_count) {
        if (backend_count == 0)
            throw std::invalid_argument("ID routing layout must contain at least one backend");
        return PAYLOAD_BITS;
    }

    GlobalId BackendEncodedId::payload_mask(const std::size_t backend_count) {
        if (backend_count == 0)
            throw std::invalid_argument("ID routing layout must contain at least one backend");
        return payload_mask();
    }

    BackendEncodedId::LocationMask BackendEncodedId::gpu_location_bit(const unsigned int device_id) {
        if (device_id >= MAX_GPU_DEVICES) {
            throw std::out_of_range("NVIDIA GPU id cannot be represented in the GlobalId location mask");
        }
        return LocationMask{1} << (GPU_BASE_BIT + device_id);
    }

    GlobalId BackendEncodedId::generate(const BackendId backend_id, const std::span<const BackendId> routing_backends) {
        GlobalId generated = invalid_global_id;
        while (generated == invalid_global_id) {
            generated = from_local_id(random_payload(), backend_id, routing_backends);
        }
        return generated;
    }

    GlobalId BackendEncodedId::encode(const GlobalId id, const BackendId backend_id,
                                      const std::span<const BackendId> routing_backends) {
        return from_local_id(local_id(id, routing_backends), backend_id, routing_backends, cache_residency(id));
    }

    GlobalId BackendEncodedId::from_local_id(const GlobalId local_id_value, const BackendId backend_id,
                                             const std::span<const BackendId> routing_backends,
                                             const LocationMask cache_residency_value) {
        validate_layout(routing_backends);
        if (!configured_owner(backend_id, routing_backends)) {
            throw std::invalid_argument("Backend is not present in the authoritative ID routing layout");
        }
        if ((local_id_value & ~payload_mask()) != 0U) {
            throw std::overflow_error("Backend-local ID does not fit in the GlobalId payload");
        }
        const LocationMask locations =
                owner_mask_for_backend(backend_id) | (cache_residency_value & cache_location_mask());
        return (static_cast<GlobalId>(locations) << PAYLOAD_BITS) | local_id_value;
    }

    GlobalId BackendEncodedId::local_id(const GlobalId id, const std::span<const BackendId> routing_backends) {
        validate_layout(routing_backends);
        return id & payload_mask();
    }

    BackendEncodedId::LocationMask BackendEncodedId::location_mask(const GlobalId id) noexcept {
        return static_cast<LocationMask>((id >> PAYLOAD_BITS) & ((GlobalId{1} << LOCATION_BITS) - 1U));
    }

    BackendEncodedId::LocationMask BackendEncodedId::owner_location(const GlobalId id) {
        const LocationMask locations = location_mask(id);
        const LocationMask persistent = locations & persistent_owner_mask();
        if (persistent != 0U) {
            if ((persistent & (persistent - 1U)) != 0U) {
                throw std::invalid_argument("Global ID contains more than one authoritative storage bit");
            }
            return persistent;
        }
        const LocationMask gpus = locations & gpu_location_mask();
        if (gpus != 0U) {
            return gpus & (~gpus + 1U); // least-significant set GPU bit
        }
        throw std::invalid_argument("Global ID contains no authoritative location bit");
    }

    BackendEncodedId::LocationMask BackendEncodedId::cache_residency(const GlobalId id) {
        const LocationMask locations = location_mask(id);
        const LocationMask owner = owner_location(id);
        return (locations & cache_location_mask()) & ~owner;
    }

    GlobalId BackendEncodedId::canonical_id(const GlobalId id) {
        return (static_cast<GlobalId>(owner_location(id)) << PAYLOAD_BITS) | (id & payload_mask());
    }

    GlobalId BackendEncodedId::with_cache_residency(const GlobalId id, const LocationMask residency) {
        const LocationMask locations = owner_location(id) | (residency & cache_location_mask());
        return (static_cast<GlobalId>(locations) << PAYLOAD_BITS) | (id & payload_mask());
    }

    bool BackendEncodedId::has_location(const GlobalId id, const LocationMask location) noexcept {
        return (location_mask(id) & location) != 0U;
    }

    BackendId BackendEncodedId::decode(const GlobalId id, const std::span<const BackendId> routing_backends) {
        validate_layout(routing_backends);
        const LocationMask owner = owner_location(id);
        BackendId backend = 0;
        if (owner == NEO4J_LOCATION)
            backend = NEO4J;
        else if (owner == ARANGO_LOCATION)
            backend = ARANGO;
        else if (owner == MEMGRAPH_LOCATION)
            backend = MEMGRAPH;
        else if (owner == POSTGRESQL_LOCATION)
            backend = POSTGRESQL;
        else if ((owner & gpu_location_mask()) != 0U) {
            unsigned int device = 0;
            LocationMask bit = gpu_location_bit(0);
            while (bit != owner && ++device < MAX_GPU_DEVICES)
                bit = gpu_location_bit(device);
            backend = gpu_backend_id(device);
        } else {
            throw std::invalid_argument("Unknown GlobalId owner location");
        }
        if (!configured_owner(backend, routing_backends)) {
            throw std::invalid_argument("Global ID owner is not enabled in the routing layout");
        }
        return backend;
    }

    std::optional<BackendId> BackendEncodedId::try_decode(const GlobalId id,
                                                          const std::span<const BackendId> routing_backends) noexcept {
        try {
            return decode(id, routing_backends);
        } catch (...) {
            return std::nullopt;
        }
    }

} // namespace polygraph::insertion
