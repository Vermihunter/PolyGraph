find_package(MPI REQUIRED COMPONENTS CXX)
find_package(CUDAToolkit REQUIRED)

enable_language(CUDA)

add_executable(polygraph-host-mpi-pingpong host_mpi_pingpong.cpp)
target_link_libraries(polygraph-host-mpi-pingpong PRIVATE MPI::MPI_CXX)
target_compile_features(polygraph-host-mpi-pingpong PRIVATE cxx_std_20)

add_executable(polygraph-cuda-aware-mpi-pingpong cuda_aware_mpi_pingpong.cu)
target_link_libraries(
    polygraph-cuda-aware-mpi-pingpong
    PRIVATE
        MPI::MPI_CXX
        CUDA::cudart
)
set_target_properties(
    polygraph-cuda-aware-mpi-pingpong
    PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED YES
)

add_executable(polygraph-mg-cugraph-bfs mg_cugraph_bfs.cpp)
target_link_libraries(
    polygraph-mg-cugraph-bfs
    PRIVATE
        polygraph::gpu_dependencies
        MPI::MPI_CXX
        CUDA::cudart
)
target_compile_features(polygraph-mg-cugraph-bfs PRIVATE cxx_std_20)
