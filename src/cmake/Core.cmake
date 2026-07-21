set(CPGDB_CORE_SOURCES
    "${PROJECT_SOURCE_DIR}/src/algorithms/ExpandSpec.cpp"
    "${PROJECT_SOURCE_DIR}/src/algorithms/bfs/BfsExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/algorithms/common/NeighborExpander.cpp"
    "${PROJECT_SOURCE_DIR}/src/data_access/IGraphStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/gpu/data_model/HostAdjacencyIndex.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/BackendRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/DatabaseContext.cpp"
    "${PROJECT_SOURCE_DIR}/src/infrastructure/GraphCoverage.cpp"
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
    "${PROJECT_SOURCE_DIR}/src/logging/EventLogger.cpp"
    "${PROJECT_SOURCE_DIR}/src/logging/EventScope.cpp"
    "${PROJECT_SOURCE_DIR}/src/logging/LoggingSetup.cpp"
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
    "${PROJECT_SOURCE_DIR}/src/utilities/logging.cpp"
)

add_library(cpgdb_core STATIC ${CPGDB_CORE_SOURCES})
add_library(cpgdb::core ALIAS cpgdb_core)

target_link_libraries(cpgdb_core PUBLIC cpgdb::model spdlog::spdlog)
if(CPGDB_ENABLE_GPU)
    target_link_libraries(cpgdb_core PUBLIC cpgdb::gpu_dependencies)
endif()

cpgdb_configure_target(cpgdb_core)
