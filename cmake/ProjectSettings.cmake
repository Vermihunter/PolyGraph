include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/lib")
endforeach()

add_library(polygraph_project_options INTERFACE)
target_compile_definitions(polygraph_project_options INTERFACE
        # Root directory -> for relative paths to root
        POLYGRAPH_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
        POLYGRAPH_VERSION="${PROJECT_VERSION}"

        # Structured execution events. 0 makes EVENT_* disappear at preprocessing time.
        POLYGRAPH_ENABLE_EVENT_GATHERING=$<BOOL:${POLYGRAPH_ENABLE_EVENT_GATHERING}>
        POLYGRAPH_ENABLE_ALGORITHM_ANALYZER=$<BOOL:${POLYGRAPH_ENABLE_ALGORITHM_ANALYZER}>

        # Compile-time event severity threshold (only relevant when gathering is enabled).
        $<$<CONFIG:Debug>:POLYGRAPH_EVENT_COMPILED_LEVEL=0>
        $<$<CONFIG:RelWithDebInfo>:POLYGRAPH_EVENT_COMPILED_LEVEL=1>
        $<$<CONFIG:Release>:POLYGRAPH_EVENT_COMPILED_LEVEL=2>
        $<$<CONFIG:MinSizeRel>:POLYGRAPH_EVENT_COMPILED_LEVEL=3>
)

add_library(polygraph::project_options ALIAS polygraph_project_options)

target_compile_features(polygraph_project_options INTERFACE cxx_std_23)
target_include_directories(
    polygraph_project_options
    INTERFACE
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

if(POLYGRAPH_ENABLE_METRICS)
    target_compile_definitions(polygraph_project_options INTERFACE POLYGRAPH_ENABLE_METRICS)
endif()

if(POLYGRAPH_USE_JEMALLOC)
    target_compile_definitions(polygraph_project_options INTERFACE POLYGRAPH_USE_JEMALLOC)
endif()

if(POLYGRAPH_ENABLE_PROFILING)
    target_compile_definitions(polygraph_project_options INTERFACE POLYGRAPH_ENABLE_PROFILING)
endif()

add_library(polygraph_project_warnings INTERFACE)
add_library(polygraph::project_warnings ALIAS polygraph_project_warnings)

target_compile_options(
    polygraph_project_warnings
    INTERFACE
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4;/permissive-;/Zc:__cplusplus>"
)

if(POLYGRAPH_WARNINGS_AS_ERRORS)
    target_compile_options(
        polygraph_project_warnings
        INTERFACE
            "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Werror>"
            "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX>"
    )
endif()
