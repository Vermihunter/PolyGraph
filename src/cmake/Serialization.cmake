set(POLYGRAPH_SERIALIZATION_SOURCES
    "${PROJECT_SOURCE_DIR}/src/serialization/BfsResultJsonSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/serialization/DataModelJsonSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/serialization/RecordJsonSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/serialization/MinimalAdjacencyRowJsonSerializer.cpp"
    "${PROJECT_SOURCE_DIR}/src/serialization/NodeRefJsonSerializer.cpp"
)

add_library(polygraph_serialization STATIC ${POLYGRAPH_SERIALIZATION_SOURCES})
add_library(polygraph::serialization ALIAS polygraph_serialization)

target_link_libraries(polygraph_serialization PUBLIC polygraph::core spdlog::spdlog)
polygraph_configure_target(polygraph_serialization)
