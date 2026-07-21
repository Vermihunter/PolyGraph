set(CPGDB_MODEL_SOURCES
    "${PROJECT_SOURCE_DIR}/src/data_model/Record.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_model/Value.cpp"
    "${PROJECT_SOURCE_DIR}/src/domain/graph/LogicalGraphSchema.cpp"
)

add_library(cpgdb_model STATIC ${CPGDB_MODEL_SOURCES})
add_library(cpgdb::model ALIAS cpgdb_model)

target_link_libraries(cpgdb_model PUBLIC spdlog::spdlog)
cpgdb_configure_target(cpgdb_model)
