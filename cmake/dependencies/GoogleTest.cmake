include_guard(GLOBAL)

find_package(GTest CONFIG QUIET)
if(TARGET GTest::gtest_main)
    message(STATUS "CPGDB dependency: GoogleTest")
    return()
endif()

if(NOT CPGDB_FETCH_TEST_DEPENDENCIES)
    message(FATAL_ERROR "Tests require GoogleTest or CPGDB_FETCH_TEST_DEPENDENCIES=ON")
endif()

include(FetchContent)
message(STATUS "CPGDB dependency: GoogleTest not installed; fetching v1.15.2")
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
