include_guard(GLOBAL)

# Build-based incremental formatting:
# - the first build formats every first-party source file;
# - later builds format only files newer than their stamp;
# - changing .clang-format reformats all files;
# - include/utilities/json.hpp is excluded because it is vendored.

if(POLYGRAPH_ENABLE_CLANG_FORMAT)
    if(NOT POLYGRAPH_CLANG_FORMAT_EXECUTABLE)
        find_program(POLYGRAPH_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
    endif()
    if(NOT POLYGRAPH_CLANG_FORMAT_EXECUTABLE)
        message(FATAL_ERROR "clang-format is enabled but was not found - ${POLYGRAPH_CLANG_FORMAT_EXECUTABLE}")
    endif()
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/.clang-format")
        message(FATAL_ERROR "clang-format is enabled but .clang-format is missing")
    endif()

    set(_polygraph_format_patterns)
    foreach(extension c cc cpp cxx h hh hpp hxx ipp tpp cu cuh)
        list(APPEND _polygraph_format_patterns
            "../../../../Projects/diplomka/vermesa/include/*.${extension}"
            "${PROJECT_SOURCE_DIR}/src/*.${extension}"
            "${PROJECT_SOURCE_DIR}/tests/*.${extension}"
        )
    endforeach()

    file(
        GLOB_RECURSE _polygraph_format_files
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES FALSE
        ${_polygraph_format_patterns}
    )
    list(FILTER _polygraph_format_files EXCLUDE REGEX "/include/utilities/json\\.hpp$")
    list(REMOVE_DUPLICATES _polygraph_format_files)
    list(SORT _polygraph_format_files)

    set(_polygraph_format_stamp_dir "${CMAKE_BINARY_DIR}/format-stamps")
    file(MAKE_DIRECTORY "${_polygraph_format_stamp_dir}")
    set(_polygraph_format_stamps)

    foreach(source_file IN LISTS _polygraph_format_files)
        file(RELATIVE_PATH relative_file "${PROJECT_SOURCE_DIR}" "${source_file}")
        string(SHA256 source_id "${relative_file}")
        set(stamp_file "${_polygraph_format_stamp_dir}/${source_id}.stamp")

        add_custom_command(
            OUTPUT "${stamp_file}"
            COMMAND "${POLYGRAPH_CLANG_FORMAT_EXECUTABLE}" --style=file -i "${source_file}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${stamp_file}"
            DEPENDS "${source_file}" "${PROJECT_SOURCE_DIR}/.clang-format"
            COMMENT "clang-format: ${relative_file}"
            VERBATIM
        )
        list(APPEND _polygraph_format_stamps "${stamp_file}")
    endforeach()

    add_custom_target(polygraph_format DEPENDS ${_polygraph_format_stamps})
    list(LENGTH _polygraph_format_files _polygraph_format_count)
    message(STATUS "PolyGraph clang-format: ${POLYGRAPH_CLANG_FORMAT_EXECUTABLE}")
    message(STATUS "PolyGraph clang-format files: ${_polygraph_format_count} (incremental)")
endif()

if(POLYGRAPH_ENABLE_CLANG_TIDY)
    if(NOT POLYGRAPH_CLANG_TIDY_EXECUTABLE)
        find_program(POLYGRAPH_CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
    endif()
    if(NOT POLYGRAPH_CLANG_TIDY_EXECUTABLE)
        message(FATAL_ERROR "clang-tidy is enabled but was not found")
    endif()

    set(POLYGRAPH_CLANG_TIDY_COMMAND
        "${POLYGRAPH_CLANG_TIDY_EXECUTABLE}"
        "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    )
    if(POLYGRAPH_CLANG_TIDY_WARNINGS_AS_ERRORS)
        list(APPEND POLYGRAPH_CLANG_TIDY_COMMAND "--warnings-as-errors=*")
    endif()

    message(STATUS "PolyGraph clang-tidy: ${POLYGRAPH_CLANG_TIDY_EXECUTABLE}")
endif()

function(polygraph_enable_project_checks target)
    if(POLYGRAPH_ENABLE_CLANG_FORMAT)
        add_dependencies("${target}" polygraph_format)
    endif()
    if(POLYGRAPH_ENABLE_CLANG_TIDY)
        set_property(TARGET "${target}" PROPERTY CXX_CLANG_TIDY "${POLYGRAPH_CLANG_TIDY_COMMAND}")
    endif()
endfunction()
