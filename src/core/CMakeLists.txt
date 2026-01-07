find_package(CURL REQUIRED)

add_executable(graph_example main.cpp)

target_link_libraries(graph_example CURL::libcurl)

target_include_directories(graph_example PRIVATE
    ${CMAKE_SOURCE_DIR}/include/utilities)
