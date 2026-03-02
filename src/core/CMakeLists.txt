find_package(CURL REQUIRED)

add_executable(graph_db main.cpp)

target_link_libraries(graph_db CURL::libcurl)

target_include_directories(graph_db PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
