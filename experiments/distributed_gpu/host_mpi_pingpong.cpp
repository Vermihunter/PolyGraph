// Host-buffer MPI ping-pong baseline for comparison with CUDA-aware MPI.

#include <mpi.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, world = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world);
    if (world != 2) {
        if (rank == 0) std::cerr << "Run with exactly two MPI ranks\n";
        MPI_Abort(MPI_COMM_WORLD, 2);
    }

    const std::size_t bytes = argc > 1 ? std::stoull(argv[1]) : (64ULL << 20U);
    const int iterations = argc > 2 ? std::stoi(argv[2]) : 100;
    if (bytes == 0 || bytes > static_cast<std::size_t>(std::numeric_limits<int>::max()) || iterations < 1) {
        if (rank == 0) std::cerr << "bytes must be 1..INT_MAX and iterations >= 1\n";
        MPI_Abort(MPI_COMM_WORLD, 3);
    }

    std::vector<std::uint8_t> buffer(bytes, static_cast<std::uint8_t>(rank));

    for (int i = 0; i < 5; ++i) {
        MPI_Sendrecv_replace(buffer.data(), static_cast<int>(bytes), MPI_BYTE,
                             1 - rank, 0, 1 - rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    const double t0 = MPI_Wtime();
    for (int i = 0; i < iterations; ++i) {
        MPI_Sendrecv_replace(buffer.data(), static_cast<int>(bytes), MPI_BYTE,
                             1 - rank, 0, 1 - rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    const double seconds = MPI_Wtime() - t0;

    if (rank == 0) {
        const double one_way_gb = static_cast<double>(bytes) * iterations / 1e9;
        std::cout << "host-mpi-pingpong bytes=" << bytes
                  << " iterations=" << iterations
                  << " elapsed_s=" << seconds
                  << " effective_one_way_GBps=" << (one_way_gb / seconds)
                  << '\n';
    }

    MPI_Finalize();
    return 0;
}
