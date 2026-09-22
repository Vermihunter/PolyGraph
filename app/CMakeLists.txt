# Application entry points are kept in independent modules so each frontend
# owns only its executable wiring while reusable implementation remains under src/.

if(POLYGRAPH_BUILD_BFS)
    add_subdirectory(bfs)
    add_subdirectory(bfs_benchmark)
endif()

if(POLYGRAPH_BUILD_HTTP)
    add_subdirectory(http_server)
endif()

if(POLYGRAPH_BUILD_GRPC)
    add_subdirectory(grpc_server)
endif()
