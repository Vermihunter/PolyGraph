set(CPGDB_BACKEND_SOURCES
    "${PROJECT_SOURCE_DIR}/src/cli/BackendConfigParser.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/ArangoDB/ArangoGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/ArangoDB/ArangoQueryVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/LocallyDeployedNeo4JResponseProcessor.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/response_processors/Neo4JResponseProcessor.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/Neo4JBoltClient.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/Neo4JQueryVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/Neo4jGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/ConfiguredGraphStoreFactory.cpp"
    "${PROJECT_SOURCE_DIR}/src/utilities/curl.cpp"
    "${PROJECT_SOURCE_DIR}/src/logging/sinks/SpdlogEventSink.cpp"
    "${PROJECT_SOURCE_DIR}/src/logging/sinks/TextEventSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/logging/sinks/JsonEventSerializer.cpp"
)

add_library(cpgdb_backends STATIC ${CPGDB_BACKEND_SOURCES})
add_library(cpgdb::backends ALIAS cpgdb_backends)

target_link_libraries(
    cpgdb_backends
    PUBLIC
        cpgdb::core
        cpgdb::serialization
        cpgdb::curl
)

if(CPGDB_ENABLE_GPU)
    target_link_libraries(cpgdb_backends PUBLIC cpgdb::gpu_dependencies)
    set_property(
        SOURCE "${PROJECT_SOURCE_DIR}/src/infrastructure/ConfiguredGraphStoreFactory.cpp"
        TARGET_DIRECTORY cpgdb_backends
        PROPERTY SKIP_LINTING ON
    )
endif()

cpgdb_configure_target(cpgdb_backends)
