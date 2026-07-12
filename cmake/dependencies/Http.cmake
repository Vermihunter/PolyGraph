include_guard(GLOBAL)

include(FetchContent)

find_package(jsoncpp CONFIG QUIET)
find_package(Drogon CONFIG QUIET)

function(_cpgdb_select_jsoncpp_target output)
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

_cpgdb_select_jsoncpp_target(_cpgdb_jsoncpp_target)

if(NOT _cpgdb_jsoncpp_target AND CPGDB_FETCH_DEPENDENCIES)
    message(STATUS "CPGDB dependency: JsonCpp not installed; fetching 1.9.6")
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
    _cpgdb_select_jsoncpp_target(_cpgdb_jsoncpp_target)
endif()

if(NOT _cpgdb_jsoncpp_target)
    message(FATAL_ERROR "The HTTP frontend requires JsonCpp")
endif()

if(TARGET Drogon::Drogon)
    set(_cpgdb_drogon_target Drogon::Drogon)
elseif(TARGET drogon)
    set(_cpgdb_drogon_target drogon)
elseif(CPGDB_FETCH_DEPENDENCIES)
    message(STATUS "CPGDB dependency: Drogon not installed; fetching v1.9.9")

    # Drogon's fallback finder understands these variables when JsonCpp was
    # built by FetchContent in the same project.
    set(JSONCPP_INCLUDE_DIRS "${jsoncpp_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    set(JSONCPP_LIBRARIES "${_cpgdb_jsoncpp_target}" CACHE STRING "" FORCE)

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
        set(_cpgdb_drogon_target Drogon::Drogon)
    elseif(TARGET drogon)
        set(_cpgdb_drogon_target drogon)
    endif()
endif()

if(NOT _cpgdb_drogon_target)
    message(FATAL_ERROR "The HTTP frontend requires Drogon")
endif()

add_library(cpgdb_jsoncpp INTERFACE)
add_library(cpgdb::jsoncpp ALIAS cpgdb_jsoncpp)
target_link_libraries(cpgdb_jsoncpp INTERFACE "${_cpgdb_jsoncpp_target}")

add_library(cpgdb_drogon INTERFACE)
add_library(cpgdb::drogon ALIAS cpgdb_drogon)
target_link_libraries(cpgdb_drogon INTERFACE "${_cpgdb_drogon_target}")

message(STATUS "CPGDB dependency: Drogon and JsonCpp")
