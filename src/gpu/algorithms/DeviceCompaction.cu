#include "gpu/algorithms/DeviceCompaction.hpp"

#include <rmm/exec_policy.hpp>
#include <thrust/sort.h>
#include <thrust/unique.h>

#include <cuda_runtime.h>

namespace polygraph::gpu_detail {
    namespace {

        template<typename T>
        __global__ void bitmap_unique_kernel(const T *input, const std::size_t count, std::uint32_t *bitmap_words,
                                             T *output, std::uint64_t *output_count) {
            for (std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
                 i += static_cast<std::size_t>(blockDim.x) * gridDim.x) {
                const auto dense = static_cast<std::uint64_t>(input[i]);
                const auto word_index = static_cast<std::size_t>(dense >> 5U);
                const auto mask = std::uint32_t{1U} << static_cast<unsigned int>(dense & 31U);
                const auto old = atomicOr(reinterpret_cast<unsigned int *>(bitmap_words + word_index), mask);
                if ((old & mask) == 0U) {
                    const auto position = atomicAdd(reinterpret_cast<unsigned long long *>(output_count), 1ULL);
                    output[position] = input[i];
                }
            }
        }

        template<typename T>
        void bitmap_unique_device_impl(const T *input, const std::size_t count, std::uint32_t *bitmap_words, T *output,
                                       std::uint64_t *output_count, const rmm::cuda_stream_view stream) {
            if (count == 0) {
                return;
            }
            constexpr int block_size = 256;
            const auto blocks_needed = (count + block_size - 1U) / block_size;
            const auto capped_blocks = blocks_needed < std::size_t{65535} ? blocks_needed : std::size_t{65535};
            const int grid_size = static_cast<int>(capped_blocks);
            bitmap_unique_kernel<<<grid_size, block_size, 0, stream.value()>>>(input, count, bitmap_words, output,
                                                                               output_count);
        }

        template<typename T>
        __global__ void mark_visited_kernel(const T *values, const std::size_t count, std::uint32_t *visited_words) {
            for (std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
                 i += static_cast<std::size_t>(blockDim.x) * gridDim.x) {
                const auto dense = static_cast<std::uint64_t>(values[i]);
                const auto word_index = static_cast<std::size_t>(dense >> 5U);
                const auto mask = std::uint32_t{1U} << static_cast<unsigned int>(dense & 31U);
                atomicOr(reinterpret_cast<unsigned int *>(visited_words + word_index), mask);
            }
        }

        template<typename T>
        void mark_visited_device_impl(const T *values, const std::size_t count, std::uint32_t *visited_words,
                                      const rmm::cuda_stream_view stream) {
            if (count == 0) {
                return;
            }
            constexpr int block_size = 256;
            const auto blocks_needed = (count + block_size - 1U) / block_size;
            const auto capped_blocks = blocks_needed < std::size_t{65535} ? blocks_needed : std::size_t{65535};
            mark_visited_kernel<<<static_cast<int>(capped_blocks), block_size, 0, stream.value()>>>(values, count,
                                                                                                    visited_words);
        }

        template<typename T>
        __global__ void filter_unvisited_kernel(const T *input, const std::size_t count, std::uint32_t *visited_words,
                                                T *output, std::uint64_t *output_count) {
            for (std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
                 i += static_cast<std::size_t>(blockDim.x) * gridDim.x) {
                const auto dense = static_cast<std::uint64_t>(input[i]);
                const auto word_index = static_cast<std::size_t>(dense >> 5U);
                const auto mask = std::uint32_t{1U} << static_cast<unsigned int>(dense & 31U);
                const auto old = atomicOr(reinterpret_cast<unsigned int *>(visited_words + word_index), mask);
                if ((old & mask) == 0U) {
                    const auto position = atomicAdd(reinterpret_cast<unsigned long long *>(output_count), 1ULL);
                    output[position] = input[i];
                }
            }
        }

        template<typename T>
        void filter_unvisited_device_impl(const T *input, const std::size_t count, std::uint32_t *visited_words,
                                          T *output, std::uint64_t *output_count, const rmm::cuda_stream_view stream) {
            if (count == 0) {
                return;
            }
            constexpr int block_size = 256;
            const auto blocks_needed = (count + block_size - 1U) / block_size;
            const auto capped_blocks = blocks_needed < std::size_t{65535} ? blocks_needed : std::size_t{65535};
            filter_unvisited_kernel<<<static_cast<int>(capped_blocks), block_size, 0, stream.value()>>>(
                    input, count, visited_words, output, output_count);
        }

        template<typename T>
        __global__ void compact_reached_bfs_kernel(const T *distances, const std::size_t count, T *output_vertices,
                                                   T *output_distances, std::uint64_t *output_count,
                                                   const T max_distance) {
            for (std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
                 i += static_cast<std::size_t>(blockDim.x) * gridDim.x) {
                const T distance = distances[i];
                if (distance >= T{0} && distance <= max_distance) {
                    const auto position = atomicAdd(reinterpret_cast<unsigned long long *>(output_count), 1ULL);
                    output_vertices[position] = static_cast<T>(i);
                    output_distances[position] = distance;
                }
            }
        }

        template<typename T>
        void compact_reached_bfs_device_impl(const T *distances, const std::size_t count, T *output_vertices,
                                             T *output_distances, std::uint64_t *output_count, const T max_distance,
                                             const rmm::cuda_stream_view stream) {
            if (count == 0) {
                return;
            }
            constexpr int block_size = 256;
            const auto blocks_needed = (count + block_size - 1U) / block_size;
            const auto capped_blocks = blocks_needed < std::size_t{65535} ? blocks_needed : std::size_t{65535};
            compact_reached_bfs_kernel<<<static_cast<int>(capped_blocks), block_size, 0, stream.value()>>>(
                    distances, count, output_vertices, output_distances, output_count, max_distance);
        }

        template<typename T>
        std::size_t sort_unique_device_impl(T *values, const std::size_t count, const rmm::cuda_stream_view stream) {
            if (count == 0) {
                return 0;
            }

            auto *const begin = values;
            auto *const end = values + count;
            auto policy = rmm::exec_policy(stream);

            thrust::sort(policy, begin, end);
            auto *const unique_end = thrust::unique(policy, begin, end);
            return static_cast<std::size_t>(unique_end - begin);
        }

    } // namespace

    std::size_t sort_unique_device(std::int32_t *values, const std::size_t count, const rmm::cuda_stream_view stream) {
        return sort_unique_device_impl(values, count, stream);
    }

    std::size_t sort_unique_device(std::int64_t *values, const std::size_t count, const rmm::cuda_stream_view stream) {
        return sort_unique_device_impl(values, count, stream);
    }

    void bitmap_unique_device(const std::int32_t *input, const std::size_t count, std::uint32_t *bitmap_words,
                              std::int32_t *output, std::uint64_t *output_count, const rmm::cuda_stream_view stream) {
        bitmap_unique_device_impl(input, count, bitmap_words, output, output_count, stream);
    }

    void bitmap_unique_device(const std::int64_t *input, const std::size_t count, std::uint32_t *bitmap_words,
                              std::int64_t *output, std::uint64_t *output_count, const rmm::cuda_stream_view stream) {
        bitmap_unique_device_impl(input, count, bitmap_words, output, output_count, stream);
    }


    void mark_visited_device(const std::int32_t *values, const std::size_t count, std::uint32_t *visited_words,
                             const rmm::cuda_stream_view stream) {
        mark_visited_device_impl(values, count, visited_words, stream);
    }

    void mark_visited_device(const std::int64_t *values, const std::size_t count, std::uint32_t *visited_words,
                             const rmm::cuda_stream_view stream) {
        mark_visited_device_impl(values, count, visited_words, stream);
    }

    void filter_unvisited_device(const std::int32_t *input, const std::size_t count, std::uint32_t *visited_words,
                                 std::int32_t *output, std::uint64_t *output_count,
                                 const rmm::cuda_stream_view stream) {
        filter_unvisited_device_impl(input, count, visited_words, output, output_count, stream);
    }

    void filter_unvisited_device(const std::int64_t *input, const std::size_t count, std::uint32_t *visited_words,
                                 std::int64_t *output, std::uint64_t *output_count,
                                 const rmm::cuda_stream_view stream) {
        filter_unvisited_device_impl(input, count, visited_words, output, output_count, stream);
    }

    void compact_reached_bfs_device(const std::int32_t *distances, const std::size_t count,
                                    std::int32_t *output_vertices, std::int32_t *output_distances,
                                    std::uint64_t *output_count, const std::int32_t max_distance,
                                    const rmm::cuda_stream_view stream) {
        compact_reached_bfs_device_impl(distances, count, output_vertices, output_distances, output_count, max_distance,
                                        stream);
    }

    void compact_reached_bfs_device(const std::int64_t *distances, const std::size_t count,
                                    std::int64_t *output_vertices, std::int64_t *output_distances,
                                    std::uint64_t *output_count, const std::int64_t max_distance,
                                    const rmm::cuda_stream_view stream) {
        compact_reached_bfs_device_impl(distances, count, output_vertices, output_distances, output_count, max_distance,
                                        stream);
    }

} // namespace polygraph::gpu_detail
