include_guard(GLOBAL)

function(polygraph_configure_target target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "polygraph_configure_target: unknown target '${target}'")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "EXECUTABLE")
        target_link_libraries("${target}" PRIVATE polygraph::project_options polygraph::project_warnings)
    else()
        target_link_libraries("${target}" PUBLIC polygraph::project_options)
        target_link_libraries("${target}" PRIVATE polygraph::project_warnings)
    endif()

    polygraph_enable_project_checks("${target}")
endfunction()
