set(CPGDB_SERIALIZATION_SOURCES
    "${PROJECT_SOURCE_DIR}/src/serialization/BfsResultJsonSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/serialization/DataModelJsonSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/serialization/RecordJsonSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/serialization/MinimalAdjacencyRowJsonSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/serialization/NodeRefJsonSerializer.cpp"
)

add_library(cpgdb_serialization STATIC ${CPGDB_SERIALIZATION_SOURCES})
add_library(cpgdb::serialization ALIAS cpgdb_serialization)

target_link_libraries(cpgdb_serialization PUBLIC cpgdb::core cpgdb::boost spdlog::spdlog)
cpgdb_configure_target(cpgdb_serialization)
