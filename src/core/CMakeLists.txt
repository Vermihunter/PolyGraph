cmake_minimum_required(VERSION 3.26)
project(cugraph_cpp_example LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CUDA_STANDARD 17)

set(CMAKE_CUDA_ARCHITECTURES 86)

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
        ../../include/GPU/GPU_Graph.hpp
        ../../include/GPU/DataModelToGPUGraphConverter.hpp
)

target_link_libraries(graph_db
        CURL::libcurl
        CUDA::cudart
        raft::raft
        rmm::rmm
        cugraph::cugraph)

target_include_directories(graph_db PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${taskflow_SOURCE_DIR}
)
