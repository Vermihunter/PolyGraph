//
// Created by akos on 8. 3. 2026.
//

#ifndef CPGDB_DATATOGPUTASK_HPP
#define CPGDB_DATATOGPUTASK_HPP

#include <raft/core/handle.hpp>

#include "gpu/DataModelToGPUGraphConverter.hpp"
#include "query/QueryResult.hpp"

template <
    typename vertex_t,
    typename edge_t,
    typename weight_t,
    typename result_t,
    bool store_transposed,
    bool multi_gpu
>
auto data_to_gpu(query_result_t results) {
    raft::handle_t handle;
    DataModelToGPUGraphConverter converter;

    return converter.convert<vertex_t, edge_t, weight_t, result_t, store_transposed, multi_gpu>(handle, results->data);
}

#endif //CPGDB_DATATOGPUTASK_HPP