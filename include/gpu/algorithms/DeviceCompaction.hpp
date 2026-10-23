/**
 * @file include/gpu/algorithms/DeviceCompaction.hpp
 * @brief CUDA-compiled helpers for device-side candidate compaction.
 */
#ifndef POLYGRAPH_DEVICE_COMPACTION_HPP
#define POLYGRAPH_DEVICE_COMPACTION_HPP

#include <cstddef>
#include <cstdint>
#include <rmm/cuda_stream_view.hpp>

namespace polygraph::gpu_detail {

/**
 * Sorts a device-resident array in-place and removes duplicates logically.
 *
 * The returned value is the number of unique elements at the beginning of
 * the same device buffer. The implementation lives in a .cu translation unit
 * so Thrust's CUDA backend is instantiated by NVCC rather than the host C++
 * compiler.
 */
std::size_t sort_unique_device(std::int32_t* values,
                               std::size_t count,
                               rmm::cuda_stream_view stream);

std::size_t sort_unique_device(std::int64_t* values,
                               std::size_t count,
                               rmm::cuda_stream_view stream);

/**
 * Deduplicates dense vertex IDs with an atomic device bitmap.
 *
 * The caller owns all temporary buffers. bitmap_words and output_count must be
 * zero-initialized before this call. Unique values are written to output in an
 * unspecified order and output_count receives the number of written values.
 */
void bitmap_unique_device(const std::int32_t* input,
                          std::size_t count,
                          std::uint32_t* bitmap_words,
                          std::int32_t* output,
                          std::uint64_t* output_count,
                          rmm::cuda_stream_view stream);

void bitmap_unique_device(const std::int64_t* input,
                          std::size_t count,
                          std::uint32_t* bitmap_words,
                          std::int64_t* output,
                          std::uint64_t* output_count,
                          rmm::cuda_stream_view stream);


/**
 * Marks dense IDs as visited in a persistent bitmap.
 */
void mark_visited_device(const std::int32_t* values,
                         std::size_t count,
                         std::uint32_t* visited_words,
                         rmm::cuda_stream_view stream);

void mark_visited_device(const std::int64_t* values,
                         std::size_t count,
                         std::uint32_t* visited_words,
                         rmm::cuda_stream_view stream);

/**
 * Filters a device-resident unique candidate array against a persistent visited
 * bitmap, atomically marks first-seen IDs, and compacts only newly seen values
 * into output. output_count must be zero-initialized by the caller.
 */
void filter_unvisited_device(const std::int32_t* input,
                             std::size_t count,
                             std::uint32_t* visited_words,
                             std::int32_t* output,
                             std::uint64_t* output_count,
                             rmm::cuda_stream_view stream);

void filter_unvisited_device(const std::int64_t* input,
                             std::size_t count,
                             std::uint32_t* visited_words,
                             std::int64_t* output,
                             std::uint64_t* output_count,
                             rmm::cuda_stream_view stream);

/**
 * Compacts reached vertices from a cuGraph BFS distance array. The current
 * Reached vertices have a non-negative distance not exceeding max_distance.
 * Checking the upper bound also excludes cuGraph builds that use max(vertex_t)
 * rather than -1 for unreachable vertices. Output order is unspecified.
 */
void compact_reached_bfs_device(const std::int32_t* distances,
                                std::size_t count,
                                std::int32_t* output_vertices,
                                std::int32_t* output_distances,
                                std::uint64_t* output_count,
                                std::int32_t max_distance,
                                rmm::cuda_stream_view stream);

void compact_reached_bfs_device(const std::int64_t* distances,
                                std::size_t count,
                                std::int64_t* output_vertices,
                                std::int64_t* output_distances,
                                std::uint64_t* output_count,
                                std::int64_t max_distance,
                                rmm::cuda_stream_view stream);

}  // namespace polygraph::gpu_detail

#endif  // POLYGRAPH_DEVICE_COMPACTION_HPP
