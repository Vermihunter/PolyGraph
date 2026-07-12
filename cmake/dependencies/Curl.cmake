include_guard(GLOBAL)

find_package(CURL REQUIRED)
set(_cpgdb_curl_target CURL::libcurl)

# Some installations expose a static libcurl target without all transitive
# libraries. pkg-config usually provides the complete Unix link line.
if(UNIX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CPGDB_LIBCURL QUIET IMPORTED_TARGET libcurl)
        if(TARGET PkgConfig::CPGDB_LIBCURL)
            set(_cpgdb_curl_target PkgConfig::CPGDB_LIBCURL)
        endif()
    endif()
endif()

add_library(cpgdb_curl INTERFACE)
add_library(cpgdb::curl ALIAS cpgdb_curl)
target_link_libraries(cpgdb_curl INTERFACE "${_cpgdb_curl_target}")

message(STATUS "CPGDB dependency: libcurl")
