add_executable(polygraph_http_server main.cpp)

target_link_libraries(
        polygraph_http_server
        PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,polygraph_http_frontend>"
        polygraph::drogon
)

polygraph_configure_target(polygraph_http_server)