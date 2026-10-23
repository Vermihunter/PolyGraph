// CUDA-aware MPI GPU-buffer ping-pong.
// If the MPI/UCX stack and platform are configured for GPUDirect RDMA, these
// device pointers can travel GPU <-> NIC without a CPU bounce buffer.

#include <cuda_runtime.h>
#include <mpi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

static void cuda_check(cudaError_t e, const char* where) {
    if (e != cudaSuccess) throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(e));
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, world = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world);
    if (world != 2) {
        if (rank == 0) std::cerr << "Run with exactly two MPI ranks\n";
        MPI_Abort(MPI_COMM_WORLD, 2);
    }

    int local_rank = 0;
    MPI_Comm local{};
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank, MPI_INFO_NULL, &local);
    MPI_Comm_rank(local, &local_rank);
    int gpu_count = 0;
    cuda_check(cudaGetDeviceCount(&gpu_count), "cudaGetDeviceCount");
    cuda_check(cudaSetDevice(local_rank % std::max(1, gpu_count)), "cudaSetDevice");

    const std::size_t bytes = argc > 1 ? std::stoull(argv[1]) : (64ULL << 20U);
    const int iterations = argc > 2 ? std::stoi(argv[2]) : 100;
    if (bytes == 0 || bytes > static_cast<std::size_t>(std::numeric_limits<int>::max()) || iterations < 1) {
        if (rank == 0) std::cerr << "bytes must be 1..INT_MAX and iterations >= 1\n";
        MPI_Abort(MPI_COMM_WORLD, 3);
    }
    std::uint8_t* d_buffer = nullptr;
    cuda_check(cudaMalloc(&d_buffer, bytes), "cudaMalloc");
    cuda_check(cudaMemset(d_buffer, rank, bytes), "cudaMemset");
    cuda_check(cudaDeviceSynchronize(), "cudaDeviceSynchronize");

    // Warm-up the registration/transport path.
    for (int i = 0; i < 5; ++i) {
        MPI_Sendrecv_replace(d_buffer, static_cast<int>(bytes), MPI_BYTE,
                             1 - rank, 0, 1 - rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    const double t0 = MPI_Wtime();
    for (int i = 0; i < iterations; ++i) {
        MPI_Sendrecv_replace(d_buffer, static_cast<int>(bytes), MPI_BYTE,
                             1 - rank, 0, 1 - rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    const double seconds = MPI_Wtime() - t0;

    if (rank == 0) {
        const double one_way_gb = static_cast<double>(bytes) * iterations / 1e9;
        std::cout << "cuda-aware-mpi-pingpong bytes=" << bytes
                  << " iterations=" << iterations
                  << " elapsed_s=" << seconds
                  << " effective_one_way_GBps=" << (one_way_gb / seconds)
                  << '\n';
    }

    cudaFree(d_buffer);
    MPI_Comm_free(&local);
    MPI_Finalize();
    return 0;
}
