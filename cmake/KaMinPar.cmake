include_guard(GLOBAL)

function(polygraph_enable_kaminpar target)
    if(NOT POLYGRAPH_INGESTION_KAMINPAR)
        return()
    endif()

    find_package(KaMinPar 3.7.3 CONFIG QUIET)
    if(NOT TARGET KaMinPar::KaMinPar)
        if(NOT POLYGRAPH_FETCH_DEPENDENCIES)
            message(FATAL_ERROR "KaMinPar >=3.7.3 is required when POLYGRAPH_INGESTION_KAMINPAR=ON. Set KaMinPar_DIR/CMAKE_PREFIX_PATH or enable POLYGRAPH_FETCH_DEPENDENCIES.")
        endif()
        include(FetchContent)
        foreach(option BUILD_APPS BUILD_DISTRIBUTED BUILD_TESTS BUILD_TOOLS BUILD_BENCHMARKS BUILD_EXAMPLES BUILD_IO)
            set(KAMINPAR_${option} OFF CACHE BOOL "Build only the shared-memory ingestion library" FORCE)
        endforeach()
        # Without KAssert, upstream's Debug fallback executes assertions while
        # KASSERT_ENABLED says initialization/checking code is disabled.
        set(KAMINPAR_BUILD_WITH_KASSERT ON CACHE BOOL "Use consistent upstream assertion levels in Debug builds" FORCE)
        set(KAMINPAR_ENABLE_TIMERS OFF CACHE BOOL "Ingestion records its own timers" FORCE)
        set(INSTALL_KAMINPAR OFF CACHE BOOL "Do not install an ingestion-private dependency" FORCE)
        FetchContent_Declare(KaMinPar
            GIT_REPOSITORY https://github.com/KaHIP/KaMinPar.git
            GIT_TAG 04dfa70cc3e6dbdf3c5fdf715d6e95beff267651
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(KaMinPar)
        FetchContent_GetProperties(KaMinPar SOURCE_DIR kaminpar_source)
        find_package(Git QUIET)
        if(Git_FOUND)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${kaminpar_source}" rev-parse HEAD
                OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif()
        if(NOT revision)
            file(SHA256 "${kaminpar_source}/include/kaminpar-shm/kaminpar.h" revision)
            set(revision "header-sha256:${revision}")
        endif()
        set(identity "source:${revision}")
    else()
        set(identity "package:${KaMinPar_VERSION}")
    endif()
    target_link_libraries(${target} PRIVATE KaMinPar::KaMinPar ${CMAKE_DL_LIBS})
    target_compile_definitions(${target} PRIVATE
        POLYGRAPH_INGESTION_HAS_KAMINPAR=1 "POLYGRAPH_KAMINPAR_IDENTITY=\"${identity}\"")
endfunction()
