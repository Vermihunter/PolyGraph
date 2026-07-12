add_library(
    cpgdb_services
    STATIC
        "${PROJECT_SOURCE_DIR}/src/server/services/TraversalService.cpp"
)
add_library(cpgdb::services ALIAS cpgdb_services)

target_link_libraries(cpgdb_services PUBLIC cpgdb::core)
cpgdb_configure_target(cpgdb_services)
