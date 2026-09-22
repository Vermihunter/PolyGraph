/**
 * @file include/utilities/BackendIds.hpp
 * @brief Stable backend ids and dynamic GPU backend-id helpers.
 */
#ifndef POLYGRAPH_BACKENDIDS_HPP
#define POLYGRAPH_BACKENDIDS_HPP

#include "infrastructure/BackendId.hpp"

namespace polygraph {
constexpr BackendId GPU = 1;          // GPU device 0 (legacy-compatible)
constexpr BackendId MEMGRAPH = 2;
constexpr BackendId NEO4J = 3;
constexpr BackendId ARANGO = 4;
constexpr BackendId REDIS = 5;
constexpr BackendId MMAP = 6;
constexpr BackendId POSTGRESQL = 7;
constexpr BackendId GPU_DEVICE_BASE = 100;

[[nodiscard]] constexpr BackendId gpu_backend_id(const unsigned int device_id) noexcept {
    return device_id == 0 ? GPU : GPU_DEVICE_BASE + device_id;
}

[[nodiscard]] constexpr bool is_gpu_backend_id(const BackendId id) noexcept {
    return id == GPU || id > GPU_DEVICE_BASE;
}

[[nodiscard]] constexpr unsigned int gpu_device_id_from_backend(const BackendId id) noexcept {
    return id == GPU ? 0U : static_cast<unsigned int>(id - GPU_DEVICE_BASE);
}

} // namespace polygraph
#endif // POLYGRAPH_BACKENDIDS_HPP
