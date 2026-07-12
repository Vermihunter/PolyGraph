set(CPGDB_HTTP_SOURCES
        "${CMAKE_SOURCE_DIR}/src/server/controllers/TraversalController.cpp"
        "${CMAKE_SOURCE_DIR}/src/server/dto/BfsRequest.cpp"
        "${CMAKE_SOURCE_DIR}/src/server/http/ErrorMapper.cpp"
        "${CMAKE_SOURCE_DIR}/src/server/http/JsonResponse.cpp"
        "${CMAKE_SOURCE_DIR}/src/server/http/RequestParsing.cpp"
        "${CMAKE_SOURCE_DIR}/src/server/services/ServicesPlugin.cpp"
)

add_library(cpgdb_http STATIC ${CPGDB_HTTP_SOURCES})
add_library(cpgdb::http ALIAS cpgdb_http)

target_link_libraries(
        cpgdb_http
        PUBLIC
        cpgdb::runtime
        cpgdb::services
        cpgdb::serialization
        cpgdb::drogon
        cpgdb::jsoncpp
)
cpgdb_configure_target(cpgdb_http)

add_executable(http_server main.cpp)
target_link_libraries(http_server PRIVATE cpgdb::http )
cpgdb_configure_target(http_server)
