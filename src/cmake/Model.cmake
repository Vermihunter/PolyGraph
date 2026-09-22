set(POLYGRAPH_MODEL_SOURCES
    "${PROJECT_SOURCE_DIR}/src/data_model/Record.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_model/Value.cpp"
    "${PROJECT_SOURCE_DIR}/src/domain/graph/LogicalGraphSchema.cpp"
)

add_library(polygraph_model STATIC ${POLYGRAPH_MODEL_SOURCES})
add_library(polygraph::model ALIAS polygraph_model)

target_link_libraries(polygraph_model PUBLIC spdlog::spdlog)
polygraph_configure_target(polygraph_model)
