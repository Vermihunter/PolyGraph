include_guard(GLOBAL)

find_package(CURL REQUIRED)
set(_polygraph_curl_target CURL::libcurl)

# Some installations expose a static libcurl target without all transitive
# libraries. pkg-config usually provides the complete Unix link line.
if(UNIX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(POLYGRAPH_LIBCURL QUIET IMPORTED_TARGET libcurl)
        if(TARGET PkgConfig::POLYGRAPH_LIBCURL)
            set(_polygraph_curl_target PkgConfig::POLYGRAPH_LIBCURL)
        endif()
    endif()
endif()

add_library(polygraph_curl INTERFACE)
add_library(polygraph::curl ALIAS polygraph_curl)
target_link_libraries(polygraph_curl INTERFACE "${_polygraph_curl_target}")

message(STATUS "PolyGraph dependency: libcurl")
