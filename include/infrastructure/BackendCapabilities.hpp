//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_BACKENDCAPABILITIES_HPP
#define CPGDB_BACKENDCAPABILITIES_HPP

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

#endif //CPGDB_BACKENDCAPABILITIES_HPP
