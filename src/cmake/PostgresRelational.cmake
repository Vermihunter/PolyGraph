if(TARGET polygraph_relational)
    return()
endif()
find_package(PostgreSQL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_library(polygraph_relational STATIC
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/PostgresRelationalNodeReader.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/RelationalMapping.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/RelationalIdentityResolver.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/RelationalAttachment.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/RelationalRelationshipTableReader.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/RelationalForeignKeyReader.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/RelationalRelationshipRead.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/RelationalMappingValidator.cpp"
)
add_library(polygraph::relational ALIAS polygraph_relational)
target_link_libraries(polygraph_relational PUBLIC polygraph::core PostgreSQL::PostgreSQL SQLite::SQLite3 OpenSSL::Crypto)
polygraph_configure_target(polygraph_relational)
