set(CPGDB_RUNTIME_SOURCES
    "${PROJECT_SOURCE_DIR}/src/cli/CommandLine.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/CurlGlobalGuard.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/GraphRuntimeBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/Cli.cpp"
)

add_library(cpgdb_runtime STATIC ${CPGDB_RUNTIME_SOURCES})
add_library(cpgdb::runtime ALIAS cpgdb_runtime)

target_link_libraries(
    cpgdb_runtime
    PUBLIC
        cpgdb::backends
        cpgdb::serialization
        spdlog::spdlog
    PRIVATE
        cpgdb::curl
)

if(CPGDB_ENABLE_GPU)
    target_link_libraries(cpgdb_runtime PRIVATE cpgdb::gpu_dependencies)
endif()

cpgdb_configure_target(cpgdb_runtime)
