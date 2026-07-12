include(GrpcProto)

cpgdb_add_grpc_proto(
        cpgdb_grpc_proto
        PROTO "${CMAKE_SOURCE_DIR}/proto/bfs_stream.proto"
)

add_library(
        cpgdb_grpc
        STATIC
        "${CMAKE_SOURCE_DIR}/src/server/grpc/GrpcTraversalService.cpp"
)
add_library(cpgdb::grpc ALIAS cpgdb_grpc)

target_link_libraries(
        cpgdb_grpc
        PUBLIC
        cpgdb::services
        cpgdb_grpc_proto
)
cpgdb_configure_target(cpgdb_grpc)

add_executable(grpc_server main.cpp)
target_link_libraries(grpc_server PRIVATE cpgdb::runtime cpgdb::grpc)
cpgdb_configure_target(grpc_server)
