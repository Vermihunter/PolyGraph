set(POLYGRAPH_RUNTIME_SOURCES
    "${PROJECT_SOURCE_DIR}/src/cli/CommandLine.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/CurlGlobalGuard.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/DomainManifest.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/GraphDataReader.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/GraphRuntimeBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/NativeNodeIdResolver.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/BfsCli.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/Cli.cpp"
)

add_library(polygraph_runtime STATIC ${POLYGRAPH_RUNTIME_SOURCES})
add_library(polygraph::runtime ALIAS polygraph_runtime)

target_link_libraries(
    polygraph_runtime
    PUBLIC
        polygraph::backends
        polygraph::serialization
        spdlog::spdlog
        argparse::argparse
    PRIVATE
        polygraph::curl
)

if(POLYGRAPH_ENABLE_GPU)
    target_link_libraries(polygraph_runtime PRIVATE polygraph::gpu_dependencies)
endif()

polygraph_configure_target(polygraph_runtime)
