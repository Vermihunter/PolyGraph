cmake_minimum_required(VERSION 3.27)

project(
        PolyGraph
        VERSION 0.1.0
        DESCRIPTION "Composable graph database runtime and service frontends"
        LANGUAGES CXX
)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Documentation is independently configurable (no CUDA, databases, or spdlog).
include(Options)
if(POLYGRAPH_DOCS_ONLY)
    if(NOT POLYGRAPH_BUILD_DOCS)
        message(FATAL_ERROR "POLYGRAPH_DOCS_ONLY requires POLYGRAPH_BUILD_DOCS=ON")
    endif()
    add_subdirectory(docs)
    return()
endif()

if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()


include(ProjectSettings)
include(Dependencies)
include(CodeQuality)
include(TargetHelpers)
include(Uftrace)

polygraph_enable_uftrace(polygraph_project_options)
polygraph_setup_spdlog()

add_subdirectory(src)
add_subdirectory(app)

if(POLYGRAPH_BUILD_EXAMPLES AND TARGET polygraph_ingestion_cli)
    add_subdirectory(examples)
endif()

if(POLYGRAPH_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING AND EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    elseif(BUILD_TESTING)
        message(FATAL_ERROR "POLYGRAPH_BUILD_TESTS=ON, but tests/CMakeLists.txt is missing")
    endif()
endif()

if(POLYGRAPH_BUILD_DOCS)
    if(EXISTS "${PROJECT_SOURCE_DIR}/docs/CMakeLists.txt")
        add_subdirectory(docs)
    else()
        message(WARNING "POLYGRAPH_BUILD_DOCS=ON, but this source archive does not contain docs/")
    endif()
endif()

include(Summary)
polygraph_print_summary()
