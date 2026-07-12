cmake_minimum_required(VERSION 3.27)

project(
        CPGDB
        VERSION 0.1.0
        DESCRIPTION "Composable graph database runtime and service frontends"
        LANGUAGES CXX
)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Options)
include(ProjectSettings)
include(Dependencies)
include(CodeQuality)
include(TargetHelpers)
include(Uftrace)

cpgdb_enable_uftrace(cpgdb_project_options)
cpgdb_setup_spdlog()

add_subdirectory(src)
add_subdirectory(app)

if(CPGDB_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()

if(CPGDB_BUILD_DOCS)
    add_subdirectory(docs)
endif()

include(Summary)
cpgdb_print_summary()
