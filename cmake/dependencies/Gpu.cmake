include_guard(GLOBAL)

enable_language(CUDA)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)
set(CMAKE_CUDA_ARCHITECTURES "${POLYGRAPH_CUDA_ARCHITECTURES}" CACHE STRING "CUDA architectures" FORCE)

find_package(CUDAToolkit REQUIRED)
find_package(raft CONFIG REQUIRED)
find_package(rmm CONFIG REQUIRED)
find_package(cugraph CONFIG REQUIRED)

add_library(polygraph_gpu_dependencies INTERFACE)
add_library(polygraph::gpu_dependencies ALIAS polygraph_gpu_dependencies)

target_compile_definitions(polygraph_gpu_dependencies INTERFACE POLYGRAPH_ENABLE_GPU)
target_link_libraries(
    polygraph_gpu_dependencies
    INTERFACE
        CUDA::cudart
        raft::raft
        rmm::rmm
        cugraph::cugraph
)

message(STATUS "PolyGraph dependency: CUDA/cuGraph (${CMAKE_CUDA_ARCHITECTURES})")
