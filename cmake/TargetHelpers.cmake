include_guard(GLOBAL)

function(cpgdb_configure_target target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "cpgdb_configure_target: unknown target '${target}'")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "EXECUTABLE")
        target_link_libraries("${target}" PRIVATE cpgdb::project_options cpgdb::project_warnings)
    else()
        target_link_libraries("${target}" PUBLIC cpgdb::project_options)
        target_link_libraries("${target}" PRIVATE cpgdb::project_warnings)
    endif()

    cpgdb_enable_project_checks("${target}")
endfunction()
