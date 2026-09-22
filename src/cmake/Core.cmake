set(POLYGRAPH_CORE_SOURCES
    "${PROJECT_SOURCE_DIR}/src/algorithms/ExpandSpec.cpp"
    "${PROJECT_SOURCE_DIR}/src/algorithms/bfs/BfsExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/algorithms/common/NeighborExpander.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/IGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/BackendRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/DatabaseContext.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/GraphCoverage.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/scheduling/BackendRequestScheduler.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/scheduling/BackendSchedulerRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/Placement.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/PlacementCatalog.cpp"
    "${PROJECT_SOURCE_DIR}/src/insertion/BackendEncodedUuid.cpp"
    "${PROJECT_SOURCE_DIR}/src/insertion/BackendLoadTracker.cpp"
    "${PROJECT_SOURCE_DIR}/src/insertion/HomeBackendSelector.cpp"
    "${PROJECT_SOURCE_DIR}/src/insertion/InsertionCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/src/insertion/InsertionItem.cpp"
    "${PROJECT_SOURCE_DIR}/src/insertion/InsertionPlacementPolicy.cpp"
    "${PROJECT_SOURCE_DIR}/src/insertion/ReplicateEverywhereInsertionPlacementPolicy.cpp"
    "${PROJECT_SOURCE_DIR}/src/insertion/TypeBasedInsertionPlacementPolicy.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/Query.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/QueryResult.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/QueryVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/query_types/GetAllEdgesQuery.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/query_types/GetAllVerticesQuery.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/query_types/GetNeighborsBatchQuery.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/query_types/GetNeighborsForVertexQuery.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/query_types/GetVertexByIdQuery.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/query_types/InsertBatchQuery.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/query_types/InsertQuery.cpp"
    "${PROJECT_SOURCE_DIR}/src/utilities/Profiling.cpp"
    "${PROJECT_SOURCE_DIR}/src/utilities/ThreadPool.cpp"
)

add_library(polygraph_core STATIC ${POLYGRAPH_CORE_SOURCES})
add_library(polygraph::core ALIAS polygraph_core)

target_link_libraries(polygraph_core PUBLIC polygraph::model polygraph::observability)
if(POLYGRAPH_ENABLE_GPU)
    target_link_libraries(polygraph_core PUBLIC polygraph::gpu_dependencies)
endif()

polygraph_configure_target(polygraph_core)
