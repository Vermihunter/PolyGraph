include_guard(GLOBAL)

include(FetchContent)

function(cpgdb_setup_spdlog)
    # Already provided by a parent project or another dependency.
    if(TARGET spdlog::spdlog)
        return()
    endif()

    # Prefer an installed package.
    find_package(spdlog CONFIG QUIET)

    if(TARGET spdlog::spdlog)
        message(STATUS "Using installed spdlog")
        return()
    endif()

    if(NOT CPGDB_FETCH_DEPENDENCIES)
        message(FATAL_ERROR
                "spdlog was not found.\n"
                "Install spdlog, provide spdlog_DIR/CMAKE_PREFIX_PATH, or configure with:\n"
                "  -DCPGDB_FETCH_DEPENDENCIES=ON"
        )
    endif()

    # Disable spdlog's own auxiliary targets.
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
            spdlog

            SYSTEM

            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG        v1.17.0
            GIT_SHALLOW    TRUE
    )

    FetchContent_MakeAvailable(spdlog)

    if(NOT TARGET spdlog::spdlog)
        message(FATAL_ERROR "spdlog was fetched but spdlog::spdlog was not created")
    endif()

    message(STATUS "Using fetched spdlog")
endfunction()