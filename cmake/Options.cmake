include_guard(GLOBAL)

# Product features
option(POLYGRAPH_ENABLE_GPU "Build the CUDA/cuGraph backend" OFF)
option(POLYGRAPH_ENABLE_PROFILING "Compile built-in profiling instrumentation" ON)
option(POLYGRAPH_ENABLE_EVENT_GATHERING "Compile structured execution-event instrumentation" ON)
option(POLYGRAPH_ENABLE_ALGORITHM_ANALYZER "Compile per-query algorithm statistics instrumentation" OFF)
option(POLYGRAPH_ENABLE_UFTRACE "Add compiler instrumentation for uftrace" OFF)
option(POLYGRAPH_ENABLE_METRICS "Build Prometheus metrics and the Drogon /metrics endpoint" OFF)
option(POLYGRAPH_USE_JEMALLOC "Link PolyGraph applications with jemalloc and expose jemalloc metrics" OFF)

option(POLYGRAPH_BUILD_BACKENDS "Build the persistent/cache backend adapters" ON)
option(POLYGRAPH_BUILD_BFS "Build the BFS command-line executable" ON)
option(POLYGRAPH_BUILD_HTTP "Build the Drogon HTTP server" OFF)
option(POLYGRAPH_BUILD_GRPC "Build the gRPC streaming server" OFF)
option(POLYGRAPH_BUILD_TESTS "Build unit and smoke tests" ON)
option(POLYGRAPH_BUILD_DOCS "Build Doxygen documentation" OFF)

# Dependency acquisition
option(
    POLYGRAPH_FETCH_DEPENDENCIES
    "Fetch optional application dependencies when they are not installed"
    ON
)
option(
    POLYGRAPH_FETCH_TEST_DEPENDENCIES
    "Fetch GoogleTest when it is not installed"
    ON
)

set(
    POLYGRAPH_DEPENDENCY_PREFIX
    ""
    CACHE PATH
    "Optional dependency prefix, for example a Conda environment"
)
set(
    POLYGRAPH_CUDA_ARCHITECTURES
    "86"
    CACHE STRING
    "CUDA architectures used by PolyGraph"
)

# Developer tools
option(
    POLYGRAPH_ENABLE_CLANG_FORMAT
    "Format new and modified first-party C/C++ files before compilation"
    ON
)
option(
    POLYGRAPH_ENABLE_CLANG_TIDY
    "Run clang-tidy during C++ compilation"
    ON
)
option(
    POLYGRAPH_CLANG_TIDY_WARNINGS_AS_ERRORS
    "Fail the build on clang-tidy diagnostics"
    ON
)
option(POLYGRAPH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(
    POLYGRAPH_CLANG_FORMAT_EXECUTABLE
    ""
    CACHE FILEPATH
    "Path to clang-format"
)
set(
    POLYGRAPH_CLANG_TIDY_EXECUTABLE
    ""
    CACHE FILEPATH
    "Path to clang-tidy"
)

# Derived feature relationships
set(POLYGRAPH_BUILD_RUNTIME OFF)
if(POLYGRAPH_BUILD_BFS OR POLYGRAPH_BUILD_HTTP OR POLYGRAPH_BUILD_GRPC)
    set(POLYGRAPH_BUILD_RUNTIME ON)
endif()

if(POLYGRAPH_BUILD_RUNTIME AND NOT POLYGRAPH_BUILD_BACKENDS)
    message(STATUS "PolyGraph: enabling backend adapters because a runtime application is enabled")
    set(POLYGRAPH_BUILD_BACKENDS ON CACHE BOOL "Build the Neo4j and ArangoDB adapters" FORCE)
endif()

if(POLYGRAPH_DEPENDENCY_PREFIX)
    list(PREPEND CMAKE_PREFIX_PATH "${POLYGRAPH_DEPENDENCY_PREFIX}")
endif()
