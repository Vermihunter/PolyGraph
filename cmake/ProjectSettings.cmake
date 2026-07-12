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

add_library(cpgdb_project_options INTERFACE)
target_compile_definitions(cpgdb_project_options INTERFACE
        # Root directory -> for relative paths to root
        CPGDB_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"

        # Logging
        $<$<CONFIG:Debug>:CPGDB_EVENT_COMPILED_LEVEL=0>
        $<$<CONFIG:RelWithDebInfo>:CPGDB_EVENT_COMPILED_LEVEL=1>
        $<$<CONFIG:Release>:CPGDB_EVENT_COMPILED_LEVEL=2>
        $<$<CONFIG:MinSizeRel>:CPGDB_EVENT_COMPILED_LEVEL=3>
)

add_library(cpgdb::project_options ALIAS cpgdb_project_options)

target_compile_features(cpgdb_project_options INTERFACE cxx_std_23)
target_include_directories(
    cpgdb_project_options
    INTERFACE
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

if(CPGDB_ENABLE_PROFILING)
    target_compile_definitions(cpgdb_project_options INTERFACE CPGDB_ENABLE_PROFILING)
endif()

add_library(cpgdb_project_warnings INTERFACE)
add_library(cpgdb::project_warnings ALIAS cpgdb_project_warnings)

target_compile_options(
    cpgdb_project_warnings
    INTERFACE
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4;/permissive-;/Zc:__cplusplus>"
)

if(CPGDB_WARNINGS_AS_ERRORS)
    target_compile_options(
        cpgdb_project_warnings
        INTERFACE
            "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Werror>"
            "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX>"
    )
endif()
