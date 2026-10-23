set(POLYGRAPH_OBSERVABILITY_SOURCES
    "${PROJECT_SOURCE_DIR}/src/logging/Logger.cpp"
    "${PROJECT_SOURCE_DIR}/src/logging/LoggingSetup.cpp"
    "${PROJECT_SOURCE_DIR}/src/observability/MetricsAccess.cpp"
    "${PROJECT_SOURCE_DIR}/src/observability/ObservabilityConfig.cpp"
)

if(POLYGRAPH_ENABLE_EVENT_GATHERING)
    list(APPEND POLYGRAPH_OBSERVABILITY_SOURCES
        "${PROJECT_SOURCE_DIR}/src/events/EventAccess.cpp"
        "${PROJECT_SOURCE_DIR}/src/events/EventCollector.cpp"
        "${PROJECT_SOURCE_DIR}/src/events/EventScope.cpp"
        "${PROJECT_SOURCE_DIR}/src/events/EventSetup.cpp"
        "${PROJECT_SOURCE_DIR}/src/events/sinks/JsonEventSerializer.cpp"
        "${PROJECT_SOURCE_DIR}/src/events/sinks/SpdlogEventSink.cpp"
        "${PROJECT_SOURCE_DIR}/src/events/sinks/TextEventSerializer.cpp"
    )
endif()

if(POLYGRAPH_ENABLE_METRICS)
    list(APPEND POLYGRAPH_OBSERVABILITY_SOURCES
        "${PROJECT_SOURCE_DIR}/src/observability/metrics/MetricsService.cpp"
        "${PROJECT_SOURCE_DIR}/src/observability/metrics/collectors/GlibcMetricsCollector.cpp"
        "${PROJECT_SOURCE_DIR}/src/observability/metrics/collectors/GpuMetricsCollector.cpp"
        "${PROJECT_SOURCE_DIR}/src/observability/metrics/collectors/JemallocMetricsCollector.cpp"
        "${PROJECT_SOURCE_DIR}/src/observability/metrics/collectors/ProcessMetricsCollector.cpp"
    )
endif()

add_library(polygraph_observability STATIC ${POLYGRAPH_OBSERVABILITY_SOURCES})
add_library(polygraph::observability ALIAS polygraph_observability)

target_link_libraries(polygraph_observability PUBLIC spdlog::spdlog)
if(POLYGRAPH_ENABLE_METRICS)
    target_link_libraries(polygraph_observability PUBLIC polygraph::prometheus)
endif()
if(POLYGRAPH_USE_JEMALLOC)
    target_link_libraries(polygraph_observability PUBLIC polygraph::jemalloc)
endif()
if(POLYGRAPH_ENABLE_METRICS AND POLYGRAPH_ENABLE_GPU)
    target_compile_definitions(polygraph_observability PRIVATE POLYGRAPH_ENABLE_GPU)
    target_link_libraries(polygraph_observability PRIVATE CUDA::nvml)
endif()

polygraph_configure_target(polygraph_observability)
