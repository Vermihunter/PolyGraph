include_guard(GLOBAL)

include(FetchContent)

find_package(argparse CONFIG QUIET)

if(NOT TARGET argparse::argparse)
    if(NOT POLYGRAPH_FETCH_DEPENDENCIES)
        message(FATAL_ERROR
            "argparse was not found. Install p-ranav/argparse, provide argparse_DIR, or set POLYGRAPH_FETCH_DEPENDENCIES=ON")
    endif()

    FetchContent_Declare(
        argparse
        GIT_REPOSITORY https://github.com/p-ranav/argparse.git
        GIT_TAG v3.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(argparse)
endif()

if(NOT TARGET argparse::argparse)
    message(FATAL_ERROR "argparse::argparse was not created")
endif()

message(STATUS "PolyGraph dependency: p-ranav/argparse")
