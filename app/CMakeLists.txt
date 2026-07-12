

if(CPGDB_BUILD_BFS)
    add_subdirectory(../app/bfs)
endif()

if(CPGDB_BUILD_HTTP)
    add_subdirectory(../app/http_server)
endif()

if(CPGDB_BUILD_GRPC)
    add_subdirectory(../app/grpc_server)
endif()
