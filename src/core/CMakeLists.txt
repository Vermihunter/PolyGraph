cmake_minimum_required(VERSION 3.26)
project(cugraph_cpp_example LANGUAGES CXX CUDA)

set(CMAKE_CXX_FLAGS "-O3 -Wall -Wextra")


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CUDA_STANDARD 17)

set(CMAKE_CUDA_ARCHITECTURES 86)

find_package(Boost CONFIG REQUIRED)



include(FetchContent)

FetchContent_Declare(
        taskflow
        GIT_REPOSITORY https://github.com/taskflow/taskflow.git
        GIT_TAG v3.7.0
)

FetchContent_MakeAvailable(taskflow)

find_package(CURL REQUIRED)
find_package(CUDAToolkit REQUIRED)
find_package(raft REQUIRED)
find_package(rmm REQUIRED)
find_package(cugraph REQUIRED)


FetchContent_Declare(
        argparse
        GIT_REPOSITORY https://github.com/p-ranav/argparse.git
        GIT_TAG v3.2
)

FetchContent_MakeAvailable(argparse)

add_executable(graph_db main.cpp
        ../../include/engine/pipeline/Task.hpp
        ../../include/engine/pipeline/Scheduler.hpp
        ../../include/serialization/DataModelJsonSerializer.hpp
        ../../include/serialization/RecordJsonSerializer.hpp
        ../../include/data_access/adapters/Neo4J/Neo4JConfig.hpp
        ../../include/data_access/adapters/Neo4J/response_processors/Neo4JResponseProcessor.hpp
        ../../include/data_access/adapters/Neo4J/response_processors/CloudNeo4JAuraDBResponseProcessor.hpp
        ../../include/data_access/adapters/Neo4J/response_processors/LocallyDeployedNeo4JResponseProcessor.hpp
        ../../include/data_access/adapters/cuGraph/cuGraphExample.hpp
        ../../include/engine/pipeline/QueryExecutionTask.hpp
        ../../include/engine/pipeline/QueryResultJoinTask.hpp
        ../../include/gpu/GPU_Graph.hpp
        ../../include/gpu/DataModelToGPUGraphConverter.hpp
        ../../include/engine/pipeline/DataToGPUTask.hpp
        ../../include/engine/pipeline/GPUPageRanksTask.hpp
        ../../include/gpu/CPU_GPU_ConversionType.hpp
        ../../include/engine/pipeline/PageRanksResultCopyBackTask.hpp
        ../../include/engine/pipeline/GPUConnectedComponentsTask.hpp
        ../../include/algorithms/bfs.hpp
        ../../include/algorithms/NeighborFinder.hpp
        ../../include/algorithms/RowByRowNeighborFinder.hpp
        ../../include/engine/NodeLocator.hpp
        ../../include/engine/GraphPartitioner.hpp
        ../../include/query/query_types/GetNeighborsForVertexQuery.hpp
        ../../include/query/query_types/GetVertexByIdQuery.hpp
        ../../include/utilities/uuid.hpp
        ../../include/domain/graph/NodeType.hpp
        ../../include/domain/graph/Property.hpp
        ../../include/domain/graph/PropertyType.hpp
        ../../include/domain/graph/EdgeType.hpp
        ../../include/domain/graph/LogicalGraphSchema.hpp
        ../../include/domain/graph/TypeId.hpp
        ../../include/domain/graph/Direction.hpp
        ../../include/infrastructure/Placement.hpp
        ../../include/infrastructure/BackendId.hpp
        ../../include/infrastructure/PlacementCatalog.hpp
        ../../include/algorithms/ExpandSpec.hpp
        ../../include/infrastructure/BackendCapabilities.hpp
        ../../include/data_access/IGraphStore.hpp
        ../../include/algorithms/bfs/BfsResult.hpp
        ../../include/algorithms/bfs/BfsExecutor.hpp
        ../../include/data_model/NeighborRef.hpp
        ../../include/data_model/AdjacencyRow.hpp
        ../../include/data_model/NodeRef.hpp
        ../../include/data_model/EdgeRef.hpp
        ../../include/data_access/adapters/Neo4J/Neo4jGraphStore.hpp
        ../../include/query/query_types/GetNeighborsBatchQuery.hpp
        ../../include/gpu/data_model/DenseIdMapper.hpp
        ../../include/gpu/data_model/CsrGraphProjection.hpp
        ../../include/infrastructure/BackendDescriptor.hpp
        ../../include/infrastructure/GraphCoverage.hpp
        ../../include/gpu/data_model/GpuProjectionBuilder.hpp
        ../../include/gpu/algorithms/CuGraphBfsExecutor.hpp
        ../../include/data_access/adapters/cuGraph/CuGraphProjectionStore.hpp
        ../../include/algorithms/bfs/BfsOptions.hpp
        ../../include/infrastructure/PlacementKind.hpp
        ../../include/gpu/data_model/HostAdjacencyIndex.hpp
        ../../include/data_access/adapters/Memgraph/MemgraphGraphStore.hpp
        ../../include/data_access/adapters/ArangoDB/ArangoGraphStore.hpp
        ../../include/data_access/adapters/Ram/RamAdjacencyStore.hpp
        main.cpp
        ../../include/infrastructure/BackendRegistry.hpp
        ../../include/infrastructure/GraphVersion.hpp
        ../../include/infrastructure/BackendConnectionConfig.hpp
        ../../include/infrastructure/DatabaseCredentials.hpp
        ../../include/infrastructure/QueryVisitorRegistry.hpp
        ../../include/utilities/BackendIds.hpp
        ../../include/gpu/GpuRuntime.hpp
        ../../include/utilities/cuda.hpp
        ../../include/gpu/algorithms/CuGraphOneHopExpandExecutor.hpp
        ../../include/utilities/Profiling.hpp
        ../../include/data_access/adapters/Neo4J/Neo4JQueryVisitorOld.hpp
        ../../include/data_model/MinimalAdjacencyRow.hpp
)

target_link_libraries(graph_db
        CURL::libcurl
        Boost::headers
        CUDA::cudart
        raft::raft
        rmm::rmm
        cugraph::cugraph
        argparse)

target_compile_definitions(graph_db PRIVATE CPGDB_ENABLE_PROFILING)

target_include_directories(graph_db PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${taskflow_SOURCE_DIR}
)
