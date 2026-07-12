include_guard(GLOBAL)

# Product features
option(CPGDB_ENABLE_GPU "Build the CUDA/cuGraph backend" OFF)
option(CPGDB_ENABLE_PROFILING "Compile built-in profiling instrumentation" ON)
option(CPGDB_ENABLE_UFTRACE "Add compiler instrumentation for uftrace" OFF)

option(CPGDB_BUILD_BACKENDS "Build the Neo4j and ArangoDB adapters" ON)
option(CPGDB_BUILD_BFS "Build the BFS command-line executable" ON)
option(CPGDB_BUILD_HTTP "Build the Drogon HTTP server" OFF)
option(CPGDB_BUILD_GRPC "Build the gRPC streaming server" OFF)
option(CPGDB_BUILD_TESTS "Build unit and smoke tests" ON)
option(CPGDB_BUILD_DOCS "Build Doxygen documentation" OFF)

# Dependency acquisition
option(
    CPGDB_FETCH_DEPENDENCIES
    "Fetch optional application dependencies when they are not installed"
    ON
)
option(
    CPGDB_FETCH_TEST_DEPENDENCIES
    "Fetch GoogleTest when it is not installed"
    ON
)

set(
    CPGDB_DEPENDENCY_PREFIX
    ""
    CACHE PATH
    "Optional dependency prefix, for example a Conda environment"
)
set(
    CPGDB_CUDA_ARCHITECTURES
    "86"
    CACHE STRING
    "CUDA architectures used by CPGDB"
)

# Developer tools
option(
    CPGDB_ENABLE_CLANG_FORMAT
    "Format new and modified first-party C/C++ files before compilation"
    ON
)
option(
    CPGDB_ENABLE_CLANG_TIDY
    "Run clang-tidy during C++ compilation"
    ON
)
option(
    CPGDB_CLANG_TIDY_WARNINGS_AS_ERRORS
    "Fail the build on clang-tidy diagnostics"
    ON
)
option(CPGDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(
    CPGDB_CLANG_FORMAT_EXECUTABLE
    ""
    CACHE FILEPATH
    "Path to clang-format"
)
set(
    CPGDB_CLANG_TIDY_EXECUTABLE
    ""
    CACHE FILEPATH
    "Path to clang-tidy"
)

# Derived feature relationships
set(CPGDB_BUILD_RUNTIME OFF)
if(CPGDB_BUILD_BFS OR CPGDB_BUILD_HTTP OR CPGDB_BUILD_GRPC)
    set(CPGDB_BUILD_RUNTIME ON)
endif()

if(CPGDB_BUILD_RUNTIME AND NOT CPGDB_BUILD_BACKENDS)
    message(STATUS "CPGDB: enabling backend adapters because a runtime application is enabled")
    set(CPGDB_BUILD_BACKENDS ON CACHE BOOL "Build the Neo4j and ArangoDB adapters" FORCE)
endif()

if(CPGDB_DEPENDENCY_PREFIX)
    list(PREPEND CMAKE_PREFIX_PATH "${CPGDB_DEPENDENCY_PREFIX}")
endif()
