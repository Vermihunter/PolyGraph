include_guard(GLOBAL)

include(FetchContent)

find_package(jsoncpp CONFIG QUIET)
find_package(Drogon CONFIG QUIET)

function(_polygraph_select_jsoncpp_target output)
    if(TARGET JsonCpp::JsonCpp)
        set(${output} JsonCpp::JsonCpp PARENT_SCOPE)
    elseif(TARGET jsoncpp_lib)
        set(${output} jsoncpp_lib PARENT_SCOPE)
    elseif(TARGET jsoncpp_static)
        set(${output} jsoncpp_static PARENT_SCOPE)
    else()
        set(${output} "" PARENT_SCOPE)
    endif()
endfunction()

_polygraph_select_jsoncpp_target(_polygraph_jsoncpp_target)

if(NOT _polygraph_jsoncpp_target AND POLYGRAPH_FETCH_DEPENDENCIES)
    message(STATUS "PolyGraph dependency: JsonCpp not installed; fetching 1.9.6")
    FetchContent_Declare(
        jsoncpp
        GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
        GIT_TAG 1.9.6
        GIT_SHALLOW TRUE
    )
    set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(jsoncpp)
    _polygraph_select_jsoncpp_target(_polygraph_jsoncpp_target)
endif()

if(NOT _polygraph_jsoncpp_target)
    message(FATAL_ERROR "The HTTP frontend requires JsonCpp")
endif()

if(TARGET Drogon::Drogon)
    set(_polygraph_drogon_target Drogon::Drogon)
elseif(TARGET drogon)
    set(_polygraph_drogon_target drogon)
elseif(POLYGRAPH_FETCH_DEPENDENCIES)
    message(STATUS "PolyGraph dependency: Drogon not installed; fetching v1.9.9")

    # Drogon's fallback finder understands these variables when JsonCpp was
    # built by FetchContent in the same project.
    get_target_property(
            JSONCPP_INCLUDE_DIRS
            "${_polygraph_jsoncpp_target}"
            INTERFACE_INCLUDE_DIRECTORIES
    )

    set(JSONCPP_INCLUDE_DIRS "${JSONCPP_INCLUDE_DIRS}" CACHE PATH "" FORCE)
    set(JSONCPP_LIBRARIES "${_polygraph_jsoncpp_target}" CACHE STRING "" FORCE)

    FetchContent_Declare(
        drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG v1.9.9
        GIT_SHALLOW TRUE
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_CTL OFF CACHE BOOL "" FORCE)
    set(BUILD_ORM OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(drogon)

    if(TARGET Drogon::Drogon)
        set(_polygraph_drogon_target Drogon::Drogon)
    elseif(TARGET drogon)
        set(_polygraph_drogon_target drogon)
    endif()
endif()

if(NOT _polygraph_drogon_target)
    message(FATAL_ERROR "The HTTP frontend requires Drogon")
endif()

add_library(polygraph_jsoncpp INTERFACE)
add_library(polygraph::jsoncpp ALIAS polygraph_jsoncpp)
target_link_libraries(polygraph_jsoncpp INTERFACE "${_polygraph_jsoncpp_target}")

add_library(polygraph_drogon INTERFACE)
add_library(polygraph::drogon ALIAS polygraph_drogon)
target_link_libraries(polygraph_drogon INTERFACE "${_polygraph_drogon_target}")

message(STATUS "PolyGraph dependency: Drogon and JsonCpp")
