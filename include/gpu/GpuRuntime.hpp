/**
 * @file include/gpu/GpuRuntime.hpp
 * @brief Declarations for gpu runtime in the GPU acceleration module.
 */

#pragma once

#ifdef CPGDB_ENABLE_GPU
#include <raft/core/handle.hpp>
#endif

namespace cpgdb {

struct GpuRuntime {
    int device_id = 0;
#ifdef CPGDB_ENABLE_GPU
    raft::handle_t handle;
#endif
};

} // namespace cpgdb
