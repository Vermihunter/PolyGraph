add_executable(polygraph_bfs main.cpp)

target_link_libraries(
    polygraph_bfs
    PRIVATE
        polygraph::runtime
        polygraph::observability
)

polygraph_configure_target(polygraph_bfs)
