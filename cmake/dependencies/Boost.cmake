include_guard(GLOBAL)

# CPGDB public headers use Boost.UUID and Boost.ContainerHash.
find_package(Boost CONFIG REQUIRED)

if(TARGET Boost::headers)
    set(_cpgdb_boost_target Boost::headers)
elseif(TARGET Boost::boost)
    set(_cpgdb_boost_target Boost::boost)
else()
    message(FATAL_ERROR "Boost was found, but no header target is available")
endif()

add_library(cpgdb_boost INTERFACE)
add_library(cpgdb::boost ALIAS cpgdb_boost)
target_link_libraries(cpgdb_boost INTERFACE "${_cpgdb_boost_target}")

message(STATUS "CPGDB dependency: Boost headers")
