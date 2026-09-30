include(cmake/Model.cmake)
include(cmake/Observability.cmake)
include(cmake/Core.cmake)
include(cmake/Serialization.cmake)

if(POLYGRAPH_BUILD_BACKENDS)
    include(cmake/Backends.cmake)
endif()

if(POLYGRAPH_BUILD_RUNTIME)
    include(cmake/Runtime.cmake)
endif()

if(POLYGRAPH_BUILD_HTTP OR POLYGRAPH_BUILD_GRPC)
    include(cmake/Services.cmake)
endif()


# Optional standalone MPI/CUDA experiments used to benchmark native cuGraph
# multi-GPU execution and CUDA-aware MPI / GPUDirect RDMA infrastructure.
# Enable with -DPOLYGRAPH_BUILD_DISTRIBUTED_GPU_EXPERIMENTS=ON.
if(POLYGRAPH_ENABLE_GPU AND POLYGRAPH_BUILD_DISTRIBUTED_GPU_EXPERIMENTS)
    add_subdirectory(
        "${PROJECT_SOURCE_DIR}/experiments/distributed_gpu"
        "${PROJECT_BINARY_DIR}/experiments/distributed_gpu"
    )
endif()
