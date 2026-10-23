include_guard(GLOBAL)

include(ExternalProject)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(JEMALLOC QUIET IMPORTED_TARGET jemalloc)
endif()

if(TARGET PkgConfig::JEMALLOC)
    set(_jemalloc_target PkgConfig::JEMALLOC)
    message(STATUS "PolyGraph dependency: jemalloc (system)")
else()
    set(JEMALLOC_ROOT "${CMAKE_BINARY_DIR}/_deps/jemalloc")
    set(JEMALLOC_INSTALL "${JEMALLOC_ROOT}/install")
    set(JEMALLOC_LIBRARY "${JEMALLOC_INSTALL}/lib/libjemalloc.a")

    # Only build if we haven't already built it.
    if(NOT EXISTS "${JEMALLOC_LIBRARY}")
        file(MAKE_DIRECTORY
                "${JEMALLOC_INSTALL}/include"
                "${JEMALLOC_INSTALL}/lib"
        )

        ExternalProject_Add(jemalloc_external
                GIT_REPOSITORY https://github.com/jemalloc/jemalloc.git
                GIT_TAG 5.3.1
                GIT_SHALLOW TRUE

                PREFIX "${JEMALLOC_ROOT}"
                BUILD_IN_SOURCE TRUE

                # We use a fixed version, so never check for updates.
                UPDATE_COMMAND ""

                CONFIGURE_COMMAND
                <SOURCE_DIR>/autogen.sh
                --prefix=${JEMALLOC_INSTALL}
                --disable-shared
                --enable-static

                BUILD_COMMAND make -j
                INSTALL_COMMAND make install

                BUILD_BYPRODUCTS "${JEMALLOC_LIBRARY}"
        )
    endif()

    add_library(jemalloc_fetched STATIC IMPORTED GLOBAL)

    set_target_properties(jemalloc_fetched PROPERTIES
            IMPORTED_LOCATION "${JEMALLOC_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${JEMALLOC_INSTALL}/include"
    )

    if(TARGET jemalloc_external)
        add_dependencies(jemalloc_fetched jemalloc_external)
    endif()

    set(_jemalloc_target jemalloc_fetched)

    message(STATUS "PolyGraph dependency: jemalloc 5.3.1 (local)")
endif()

add_library(polygraph_jemalloc INTERFACE)
add_library(polygraph::jemalloc ALIAS polygraph_jemalloc)

target_link_libraries(
        polygraph_jemalloc
        INTERFACE ${_jemalloc_target}
)