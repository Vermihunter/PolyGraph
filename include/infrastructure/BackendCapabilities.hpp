/**
 * @file include/infrastructure/BackendCapabilities.hpp
 * @brief Declarations for backend capabilities in the backend infrastructure module.
 */

//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_BACKENDCAPABILITIES_HPP
#define CPGDB_BACKENDCAPABILITIES_HPP

namespace cpgdb {
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

} // namespace cpgdb

#endif // CPGDB_BACKENDCAPABILITIES_HPP
