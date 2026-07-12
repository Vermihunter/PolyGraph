add_library(cpgdb_cli STATIC "${CMAKE_SOURCE_DIR}/src/cli/BfsCli.cpp")
add_library(cpgdb::cli ALIAS cpgdb_cli)
target_link_libraries(cpgdb_cli PUBLIC cpgdb::runtime)
cpgdb_enable_project_checks(cpgdb_cli)

target_compile_definitions(
        cpgdb_cli
        PRIVATE
        CPGDB_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

add_executable(bfs main.cpp)
target_link_libraries(bfs PRIVATE cpgdb::cli)
#cpgdb_enable_uftrace(bfs)
cpgdb_enable_project_checks(bfs)


