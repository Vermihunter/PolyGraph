cmake_minimum_required(VERSION 3.26)

project(CPGDB)

#set(CMAKE_CXX_STANDARD 23)


# ------------------------------------------------------------
# Global language settings
# ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

set(CMAKE_CUDA_ARCHITECTURES 86)

# Important because Drogon builds a shared lib and may link JsonCpp static.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Put all final executables into one clean place:
#   cmake-build-release/bin/bfs
#   cmake-build-release/bin/http_server
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# For multi-config generators. Not needed for Ninja single-config,
# but harmless.
foreach(CONFIG_TYPE DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# ------------------------------------------------------------
# Compiler flags
# ------------------------------------------------------------
add_compile_options(
        -Wall
        -Wextra
)

# ------------------------------------------------------------
# System / installed dependencies
# ------------------------------------------------------------
find_package(Boost CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(CUDAToolkit REQUIRED)
find_package(raft REQUIRED)
find_package(rmm REQUIRED)
find_package(cugraph REQUIRED)

# ------------------------------------------------------------
# FetchContent dependencies
# ------------------------------------------------------------
include(FetchContent)

# -----------------------------
# Taskflow
# -----------------------------
FetchContent_Declare(
        taskflow
        GIT_REPOSITORY https://github.com/taskflow/taskflow.git
        GIT_TAG v3.7.0
)

FetchContent_MakeAvailable(taskflow)

# -----------------------------
# argparse
# -----------------------------
FetchContent_Declare(
        argparse
        GIT_REPOSITORY https://github.com/p-ranav/argparse.git
        GIT_TAG v3.2
)

FetchContent_MakeAvailable(argparse)

# -----------------------------
# JsonCpp - needed internally by Drogon
# -----------------------------
FetchContent_Declare(
        jsoncpp
        GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
        GIT_TAG 1.9.6
)

set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(jsoncpp)

if(TARGET jsoncpp_static)
    set_target_properties(jsoncpp_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(CPGDB_JSONCPP_TARGET jsoncpp_static)
elseif(TARGET jsoncpp_lib)
    set_target_properties(jsoncpp_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(CPGDB_JSONCPP_TARGET jsoncpp_lib)
else()
    message(FATAL_ERROR "JsonCpp was fetched, but no usable jsoncpp target was created")
endif()

# Drogon's FindJsoncpp.cmake expects these old-style variables.
set(JSONCPP_INCLUDE_DIRS
        "${jsoncpp_SOURCE_DIR}/include"
        CACHE PATH "JsonCpp include dir" FORCE
)

set(JSONCPP_INCLUDE_DIR
        "${jsoncpp_SOURCE_DIR}/include"
        CACHE PATH "JsonCpp include dir" FORCE
)

set(JSONCPP_LIBRARIES
        "${CPGDB_JSONCPP_TARGET}"
        CACHE STRING "JsonCpp library target" FORCE
)

set(JSONCPP_LIBRARY
        "${CPGDB_JSONCPP_TARGET}"
        CACHE STRING "JsonCpp library target" FORCE
)

# -----------------------------
# Drogon
# -----------------------------
FetchContent_Declare(
        drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG v1.9.9
)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_CTL OFF CACHE BOOL "" FORCE)
set(BUILD_ORM OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(drogon)

# ------------------------------------------------------------
# Shared project-wide interface target
# ------------------------------------------------------------
add_library(cpgdb_common INTERFACE
        include/cli/BfsCli.hpp
        include/cli/CurlGlobalGuard.hpp
        include/cli/GraphRuntimeBuilder.hpp
        include/algorithms/bfs/BfsLayer.hpp
        include/server/grpc/GrpcTraversalService.hpp)
#        include/server/http/JsonResponse.hpp
#        include/server/http/ErrorMapper.hpp
#        include/server/http/RequestParsing.hpp
#        include/server/controllers/TraversalController.hpp
#        include/server/services/ServicesPlugin.hpp
#        include/server/services/TraversalService.hpp
#        include/server/dto/BfsRequest.hpp
#        include/serialization/BfsResultJsonSerializer.hpp
#
#)


target_include_directories(cpgdb_common INTERFACE
        "${PROJECT_SOURCE_DIR}/include"
        "${taskflow_SOURCE_DIR}"
)

target_compile_definitions(cpgdb_common INTERFACE
        CPGDB_ENABLE_PROFILING
)

target_link_libraries(cpgdb_common INTERFACE
        CURL::libcurl
        Boost::headers
        CUDA::cudart
        raft::raft
        rmm::rmm
        cugraph::cugraph
        argparse
)


# ------------------------------------------------------------
# Subprojects
# ------------------------------------------------------------
add_subdirectory(src/bfs)
add_subdirectory(src/http_server)
add_subdirectory(src/grpc_server)


#add_subdirectory(src)