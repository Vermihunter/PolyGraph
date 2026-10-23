include_guard(GLOBAL)

include(FetchContent)

find_package(prometheus-cpp CONFIG QUIET)

if(NOT TARGET prometheus-cpp::core)
    if(NOT POLYGRAPH_FETCH_DEPENDENCIES)
        message(FATAL_ERROR
            "prometheus-cpp was not found. Install it, provide prometheus-cpp_DIR, or set POLYGRAPH_FETCH_DEPENDENCIES=ON")
    endif()

    set(ENABLE_PULL OFF CACHE BOOL "" FORCE)
    set(ENABLE_PUSH OFF CACHE BOOL "" FORCE)
    set(ENABLE_COMPRESSION OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(USE_THIRDPARTY_LIBRARIES ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        prometheus_cpp
        GIT_REPOSITORY https://github.com/jupp0r/prometheus-cpp.git
        GIT_TAG v1.2.4
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_MakeAvailable(prometheus_cpp)
endif()

if(NOT TARGET prometheus-cpp::core)
    message(FATAL_ERROR "prometheus-cpp::core was not created")
endif()

add_library(polygraph_prometheus INTERFACE)
add_library(polygraph::prometheus ALIAS polygraph_prometheus)
target_link_libraries(polygraph_prometheus INTERFACE prometheus-cpp::core)

message(STATUS "PolyGraph dependency: prometheus-cpp")
