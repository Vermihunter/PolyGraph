//
// Created by akos on 28. 5. 2026.
//

#ifndef CPGDB_GPURUNTIME_HPP
#define CPGDB_GPURUNTIME_HPP

#include <raft/core/handle.hpp>

struct GpuRuntime {
    int device_id = 0;
    raft::handle_t handle;
};

#endif //CPGDB_GPURUNTIME_HPP