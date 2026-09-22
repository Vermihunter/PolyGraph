/**
 * @file include/infrastructure/BackendCapabilities.hpp
 * @brief Declarations for backend capabilities in the backend infrastructure module.
 */

//
// Created by akos on 30. 4. 2026.
//

#ifndef POLYGRAPH_BACKENDCAPABILITIES_HPP
#define POLYGRAPH_BACKENDCAPABILITIES_HPP

namespace polygraph {
/**
 * What a given backend can do: batch neighbors, native BFS, GPU execution, filters, etc
 */
struct BackendCapabilities {
    bool supports_batch_neighbors = false;
    bool supports_native_bfs = false;
    bool supports_native_property_filters = false;
    bool supports_gpu_execution = false;
    bool supports_replica_parallelism = false;
};

} // namespace polygraph

#endif // POLYGRAPH_BACKENDCAPABILITIES_HPP
