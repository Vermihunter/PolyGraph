set(POLYGRAPH_BACKEND_SOURCES
    "${PROJECT_SOURCE_DIR}/src/cli/BackendConfigParser.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/ArangoDB/ArangoGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/ArangoDB/ArangoQueryVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/LocallyDeployedNeo4JResponseProcessor.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/response_processors/Neo4JResponseProcessor.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/Neo4JBoltClient.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/Neo4JQueryVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/Neo4J/Neo4jGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/PostgreSQL/PostgreSqlGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/cache/MMapGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/adapters/cache/RedisGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/ConfiguredGraphStoreFactory.cpp"
    "${PROJECT_SOURCE_DIR}/src/utilities/curl.cpp"
)

add_library(polygraph_backends STATIC ${POLYGRAPH_BACKEND_SOURCES})
add_library(polygraph::backends ALIAS polygraph_backends)

target_link_libraries(
    polygraph_backends
    PUBLIC
        polygraph::core
        polygraph::serialization
        polygraph::curl
        PostgreSQL::PostgreSQL
)

if(POLYGRAPH_ENABLE_GPU)
    target_sources(polygraph_backends PRIVATE
        "${PROJECT_SOURCE_DIR}/src/gpu/algorithms/DeviceCompaction.cu"
    )
    target_link_libraries(polygraph_backends PUBLIC polygraph::gpu_dependencies)
    set_property(
        SOURCE "${PROJECT_SOURCE_DIR}/src/infrastructure/ConfiguredGraphStoreFactory.cpp"
        TARGET_DIRECTORY polygraph_backends
        PROPERTY SKIP_LINTING ON
    )
endif()

polygraph_configure_target(polygraph_backends)
