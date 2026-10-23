add_executable(
    polygraph_bfs_benchmark
    main.cpp
    BfsBenchmark.cpp
)

target_link_libraries(
    polygraph_bfs_benchmark
    PRIVATE
        polygraph::runtime
        polygraph::observability
)

polygraph_configure_target(polygraph_bfs_benchmark)
