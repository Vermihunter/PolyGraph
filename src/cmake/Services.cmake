add_library(
    polygraph_services
    STATIC
        "${PROJECT_SOURCE_DIR}/src/server/services/TraversalService.cpp"
)
add_library(polygraph::services ALIAS polygraph_services)
target_link_libraries(polygraph_services PUBLIC polygraph::core)
polygraph_configure_target(polygraph_services)

if(POLYGRAPH_BUILD_HTTP)
    set(polygraph_http_sources
        "${PROJECT_SOURCE_DIR}/src/server/controllers/TraversalController.cpp"
        "${PROJECT_SOURCE_DIR}/src/server/dto/BfsRequest.cpp"
        "${PROJECT_SOURCE_DIR}/src/server/http/ErrorMapper.cpp"
        "${PROJECT_SOURCE_DIR}/src/server/http/JsonResponse.cpp"
        "${PROJECT_SOURCE_DIR}/src/server/http/RequestParsing.cpp"
        "${PROJECT_SOURCE_DIR}/src/server/services/ServicesPlugin.cpp"
    )
    if(POLYGRAPH_ENABLE_METRICS)
        list(APPEND polygraph_http_sources
            "${PROJECT_SOURCE_DIR}/src/server/controllers/MetricsController.cpp"
        )
    endif()

    add_library(polygraph_http_frontend STATIC ${polygraph_http_sources})
    add_library(polygraph::http_frontend ALIAS polygraph_http_frontend)
    target_link_libraries(
        polygraph_http_frontend
        PUBLIC
            polygraph::runtime
            polygraph::services
            polygraph::drogon
            polygraph::jsoncpp
            polygraph::observability
    )
    polygraph_configure_target(polygraph_http_frontend)
endif()

if(POLYGRAPH_BUILD_GRPC)
    include(GrpcProto)
    polygraph_add_grpc_proto(
        polygraph_grpc_proto
        PROTO "${PROJECT_SOURCE_DIR}/proto/bfs_stream.proto"
    )

    add_library(
        polygraph_grpc_frontend
        STATIC
            "${PROJECT_SOURCE_DIR}/src/server/grpc/GrpcTraversalService.cpp"
    )
    add_library(polygraph::grpc_frontend ALIAS polygraph_grpc_frontend)
    target_link_libraries(
        polygraph_grpc_frontend
        PUBLIC
            polygraph::runtime
            polygraph::services
            polygraph::grpc
            polygraph_grpc_proto
    )
    polygraph_configure_target(polygraph_grpc_frontend)
endif()
