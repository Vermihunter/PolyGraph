include_guard(GLOBAL)

option(
        ENABLE_UFTRACE
        "Enable uftrace function instrumentation"
        OFF
)

add_library(cpgdb_uftrace_options INTERFACE)
add_library(cpgdb::uftrace ALIAS cpgdb_uftrace_options)

if (ENABLE_UFTRACE)
    message(STATUS "uftrace instrumentation enabled")

    target_compile_options(
            cpgdb_uftrace_options
            INTERFACE
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:
            -g
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -fpatchable-function-entry=5
            >
    )
endif()

function(cpgdb_enable_uftrace target_name)
    if (NOT TARGET "${target_name}")
        message(
                FATAL_ERROR
                "Cannot enable uftrace: target '${target_name}' does not exist"
        )
    endif()

    get_target_property(target_type "${target_name}" TYPE)

    if (target_type STREQUAL "INTERFACE_LIBRARY")
        target_link_libraries(
                "${target_name}"
                INTERFACE
                cpgdb::uftrace
        )
    else()
        target_link_libraries(
                "${target_name}"
                PRIVATE
                cpgdb::uftrace
        )
    endif()
endfunction()