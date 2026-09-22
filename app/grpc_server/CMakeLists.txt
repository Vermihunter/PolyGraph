add_executable(polygraph_grpc_server main.cpp)

target_link_libraries(
    polygraph_grpc_server
    PRIVATE
        polygraph::grpc_frontend
)

polygraph_configure_target(polygraph_grpc_server)
